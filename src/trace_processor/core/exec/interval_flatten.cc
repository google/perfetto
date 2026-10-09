/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/core/exec/interval_flatten.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/bits.h"
#include "perfetto/ext/base/no_destructor.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/util/heap.h"

namespace perfetto::trace_processor::core::exec {
namespace {

bool IsInt64(const RowBatch& batch, uint32_t index) {
  const ColumnView& column = batch.column(index);
  return column.kind() == ColumnView::Kind::kFlat && column.type().Is<Int64>();
}

// `function`'s result for each of `groups`, written into a buffer taken from
// `context` for the batch served to hold.
ColumnBuffer Finalized(Context& context,
                       const AggregateFunction& function,
                       GroupStates states,
                       const uint32_t* groups,
                       uint32_t count) {
  ColumnBuffer buffer = context.TakeBuffer();
  ColumnChunk& chunk = buffer.chunk();
  chunk.validity.resize(kMaxBatchRows);
  function.Finalize(states, groups, count, chunk.Values<int64_t>(),
                    &chunk.validity);
  return buffer;
}

constexpr auto kEndsLater = [](const auto& a, const auto& b) {
  return a.end > b.end;
};

template <typename Live>
void PushEnd(FlexVector<Live>& heap, Live live) {
  heap.push_back(live);
  HeapSiftUp(heap.data(), heap.size() - 1, live, kEndsLater);
}

template <typename Live>
void PopEnd(FlexVector<Live>& heap) {
  Live last = heap.back();
  heap.pop_back();
  if (!heap.empty()) {
    HeapSiftDown(heap.data(), heap.size(), last, kEndsLater);
  }
}

// The groups of IntervalFlatten::State::states: the nodes of the tree (see
// AggregateTree), node k being group k from 1, or the deltas (see
// AggregateDeltas), then a slot for each live interval.
constexpr uint32_t kFirstSlot = 2 * kMaxBatchRows;

// The same for every tree, so built once.
struct Tree {
  Tree() {
    for (uint32_t k = 0; k < kFirstSlot; ++k) {
      groups.push_back(k);
      if (k >= 2) {
        pushes.push_back(GroupMerge{k, k / 2});
      }
    }
  }
  // Group k is groups[k]: the leaves of a tree of n are groups + n.
  std::vector<uint32_t> groups;
  // Each node below the root merged from its parent, parents first.
  std::vector<GroupMerge> pushes;
};

const Tree& GetTree() {
  static const base::NoDestructor<Tree> tree;
  return tree.ref();
}

}  // namespace

IntervalFlatten::IntervalFlatten(IntervalFlattenSpec spec)
    : Operator({BatchPreference::kThroughput}), spec_(std::move(spec)) {
  subtract_ = true;
  for (const AggregateCall& call : spec_.aggregates) {
    functions_.push_back(MakeAggregateFunction(call.function));
    subtract_ &= functions_.back()->can_subtract();
    offsets_.push_back(stride_);
    stride_ += functions_.back()->state_words();
  }
}

IntervalFlatten::~IntervalFlatten() = default;
IntervalFlatten::State::~State() = default;

std::unique_ptr<OperatorState> IntervalFlatten::MakeState(
    Context& context) const {
  auto state = std::make_unique<State>();
  state->context = &context;
  state->seen_bytes.resize(functions_.size());
  state->outputs.resize(functions_.size());
  state->stride = stride_;
  state->Reset();
  return state;
}

OpResult IntervalFlatten::Execute(const RowBatch& in,
                                  RowBatch& out,
                                  OperatorState& state) const {
  auto& s = static_cast<State&>(state);
  out.Reset();
  s.segments = 0;
  if (!s.status.ok()) {
    return OpResult::kError;
  }
  bool typed = IsInt64(in, spec_.ts_column) && IsInt64(in, spec_.dur_column);
  for (uint32_t i = 0; i < functions_.size(); ++i) {
    typed = typed && (!functions_[i]->reads_input() ||
                      IsInt64(in, spec_.aggregates[i].column));
  }
  if (!typed) {
    s.status = base::ErrStatus(
        "INTERVAL FLATTEN: ts, dur and aggregated columns must be Int64");
    return OpResult::kError;
  }
  FlatColumnReader<int64_t> ts(in, spec_.ts_column);
  FlatColumnReader<int64_t> dur(in, spec_.dur_column);
  // A full output batch can interrupt consumption of the same input. Keep its
  // keys in place until all rows have been consumed.
  if (!s.input_pending) {
    if (!RetainKeys(in, s)) {
      return OpResult::kError;
    }
    s.input_pending = true;
  }
  uint32_t rows = in.size();
  bool keyed = !spec_.key_columns.empty();
  for (; s.input_row < rows; ++s.input_row) {
    uint32_t row = s.input_row;
    int64_t start;
    int64_t length;
    if (!ts.Read(row, &start) || !dur.Read(row, &length)) {
      continue;
    }
    if (start < 0 || length < 0) {
      s.status =
          base::ErrStatus("INTERVAL FLATTEN: a row's ts or dur is below zero");
      return OpResult::kError;
    }
    uint32_t group = keyed ? in.Value<uint32_t>(spec_.group_column, row) : 0;
    if (!s.in_group || group != s.input_group) {
      if (s.in_group) {
        // Finish the old group before accepting any rows from the new one.
        // Several small groups can still share one output batch.
        if (!Advance(s, std::numeric_limits<int64_t>::max())) {
          return Yield(out, s, OpResult::kHaveMoreOutput, &in);
        }
        ++s.output_group;
      }
      s.in_group = true;
      s.input_group = group;
      s.key_row = s.first_key + row;
      s.input_ts = std::numeric_limits<int64_t>::min();
    }
    if (start != s.input_ts) {
      if (start < s.input_ts) {
        s.status = base::ErrStatus(
            "INTERVAL FLATTEN: a group's rows must arrive in order of ts");
        return OpResult::kError;
      }
      if (!Advance(s, start)) {
        return Yield(out, s, OpResult::kHaveMoreOutput, &in);
      }
      s.input_ts = start;
    }
    if (!AddInterval(s, row, start, length)) {
      return OpResult::kError;
    }
  }
  s.input_row = 0;
  s.input_pending = false;
  // Only the current group's key must survive reuse of the input buffers.
  if (keyed && s.in_group) {
    s.key_rows.View(&s.saved_key, s.key_row, 1);
  }
  return Yield(out, s, OpResult::kNeedMoreInput, &in);
}

// The interval's state goes into a slot of its own, kept until it ends.
PERFETTO_ALWAYS_INLINE bool IntervalFlatten::AddInterval(State& s,
                                                         uint32_t row,
                                                         int64_t start,
                                                         int64_t length) const {
  uint32_t slot = TakeSlot(s);
  s.rows.push_back(row);
  s.row_slots.push_back(slot);
  if (length == 0) {
    // Counted only by the point at its time, emitted when the time is done.
    s.instants.push_back(Live{start, slot, s.segments});
    return true;
  }
  int64_t end;
  if (!base::CheckedAdd(start, length, &end)) {
    s.status = base::ErrStatus("INTERVAL FLATTEN: integer overflow");
    return false;
  }
  PushEnd(s.ends, Live{end, slot, s.segments});
  ++s.live;
  return true;
}

PERFETTO_ALWAYS_INLINE uint32_t IntervalFlatten::TakeSlot(State& s) const {
  if (s.free_slots.empty()) {
    s.states.push_back_multiple(0, stride_);
    return kFirstSlot + s.slots++;
  }
  uint32_t slot = s.free_slots.back();
  s.free_slots.pop_back();
  return slot;
}

bool IntervalFlatten::RetainKeys(const RowBatch& in, State& s) const {
  if (spec_.key_columns.empty()) {
    return true;
  }
  s.key_rows.Clear();
  // The sweep may still emit rows for the group from the previous input, even
  // after that input's borrowed buffers have been reused.
  if (s.saved_key.size()) {
    s.status = s.key_rows.Append(s.saved_key);
    if (!s.status.ok()) {
      return false;
    }
  }
  s.key_row = 0;
  s.first_key = s.key_rows.size();
  s.retained.Project(in, spec_.key_columns);
  s.status = s.key_rows.Append(s.retained);
  return s.status.ok();
}

bool IntervalFlatten::Advance(State& s, int64_t time) const {
  // Points at the last input timestamp include all rows starting there, so
  // emit them only once that timestamp is complete.
  if (!s.instants.empty()) {
    if (!Emit(s, s.input_ts, 0)) {
      return false;
    }
    for (const Live& instant : s.instants) {
      End(s, instant);
    }
    s.instants.clear();
  }
  int64_t& cursor = s.cursor;
  while (!s.ends.empty() && s.ends.begin()->end <= time) {
    int64_t end = s.ends.begin()->end;
    if (s.live > 0 && cursor < end) {
      if (!Emit(s, cursor, end - cursor)) {
        return false;
      }
    }
    cursor = end;
    Expire(s, end);
  }
  if (s.live > 0 && cursor < time) {
    if (!Emit(s, cursor, time - cursor)) {
      return false;
    }
  }
  s.cursor = time;
  return true;
}

PERFETTO_ALWAYS_INLINE void IntervalFlatten::Expire(State& s,
                                                    int64_t end) const {
  while (!s.ends.empty() && s.ends.begin()->end == end) {
    Live ended = *s.ends.begin();
    PopEnd(s.ends);
    --s.live;
    End(s, ended);
  }
}

// Records the segments of the batch an interval was live in, up to the last
// emitted; its slot is freed once they're aggregated.
PERFETTO_ALWAYS_INLINE void IntervalFlatten::End(State& s,
                                                 const Live& live) const {
  s.ranges.push_back(Range{live.slot, live.first, s.segments});
}

bool IntervalFlatten::Emit(State& s, int64_t ts, int64_t dur) const {
  // A full batch leaves the pending segment untouched. Advance can resume
  // by retrying it, without tracking a partially processed boundary.
  uint32_t n = s.segments;
  if (n == kMaxBatchRows) {
    return false;
  }
  if (PERFETTO_UNLIKELY(n == s.segment_capacity)) {
    GrowSegments(s);
  }
  if (PERFETTO_UNLIKELY(!s.ts_buffer)) {
    TakeSegmentBuffers(s);
  }
  s.segment_ts[n] = ts;
  s.segment_dur[n] = dur;
  s.segment_groups[n] = s.output_group;
  s.segment_key_rows[n] = s.key_row;
  s.segments = n + 1;
  return true;
}

PERFETTO_NO_INLINE void IntervalFlatten::GrowSegments(State& s) const {
  s.segment_capacity = std::max(64u, s.segment_capacity * 2);
  s.segment_key_rows.resize(s.segment_capacity);
}

PERFETTO_NO_INLINE void IntervalFlatten::TakeSegmentBuffers(State& s) const {
  s.ts_buffer = s.context->TakeBuffer();
  s.dur_buffer = s.context->TakeBuffer();
  s.group_buffer = s.context->TakeBuffer();
  s.segment_ts = s.ts_buffer.chunk().Values<int64_t>();
  s.segment_dur = s.dur_buffer.chunk().Values<int64_t>();
  s.segment_groups = s.group_buffer.chunk().Values<uint32_t>();
}

// Gives the batch's segments their aggregates, a function at a time: adds
// each new interval into its slot, then merges the slots of the intervals
// live in each segment, either as running totals (see AggregateDeltas) or with
// a tree (see AggregateTree).
bool IntervalFlatten::Apply(State& s, const RowBatch* in) const {
  // The ranges of the intervals which ended, then of those still live: these
  // run to the batch's end, and on from the start of the next.
  auto ended = static_cast<uint32_t>(s.ranges.size());
  for (FlexVector<Live>* lives : {&s.ends, &s.instants}) {
    for (Live& live : *lives) {
      s.ranges.push_back(Range{live.slot, live.first, s.segments});
      live.first = 0;
    }
  }
  auto rows = static_cast<uint32_t>(s.rows.size());
  bool overflow = false;
  bool nulls = false;
  for (uint32_t i = 0; i < functions_.size(); ++i) {
    const AggregateFunction& function = *functions_[i];
    AggregateInput input;
    input.rows = rows;
    if (rows && function.reads_input()) {
      s.input.Load(in->column(spec_.aggregates[i].column), in->selection(),
                   s.rows.data(), rows, &input);
    }
    GroupStates states = States(s, i, input.valid != nullptr);
    if (states.seen) {
      nulls = true;
      for (uint32_t slot : s.row_slots) {
        states.seen[slot] = 0;
      }
    }
    function.Update(input, s.row_slots.data(), states, &overflow);
  }
  // Running totals are exact, but their deltas can overflow where the totals
  // don't: the tree then decides.
  if (overflow || !subtract_ || nulls || !AggregateDeltas(s)) {
    AggregateTree(s, &overflow);
  }
  // Ended intervals' slots are zeroed for reuse, a word at a time: a few
  // words a slot would otherwise be a memset call each.
  for (uint32_t w = 0; w < stride_; ++w) {
    for (uint32_t r = 0; r < ended; ++r) {
      s.states[uint64_t{s.ranges[r].slot} * stride_ + w] = 0;
    }
  }
  for (uint32_t r = 0; r < ended; ++r) {
    s.free_slots.push_back(s.ranges[r].slot);
  }
  s.ranges.clear();
  s.rows.clear();
  s.row_slots.clear();
  if (overflow) {
    s.status = base::ErrStatus("INTERVAL FLATTEN: integer overflow");
    return false;
  }
  return true;
}

// Function i's states, with its `seen` bytes if its input has held nulls, as
// it has if `nulls`.
GroupStates IntervalFlatten::States(State& s, uint32_t i, bool nulls) const {
  GroupStates states{s.states.data() + offsets_[i], stride_};
  if (functions_[i]->tracks_seen()) {
    states.seen = s.seen_bytes[i].For(kFirstSlot + s.slots, nulls);
  }
  return states;
}

// For functions which can subtract, while no input has held nulls (so every
// segment, which has an interval live, holds a value): each interval adds its
// slot into a delta at its first segment and takes it out at its end, then
// running totals over the deltas give the segments' aggregates. Returns false,
// leaving nothing written, if the arithmetic overflowed.
bool IntervalFlatten::AggregateDeltas(State& s) const {
  uint32_t count = s.segments;
  std::fill_n(s.states.data(), uint64_t{count + 1} * stride_, 0);
  s.merges.clear();
  s.subtracts.clear();
  for (const Range& range : s.ranges) {
    if (range.first < range.end) {
      s.merges.push_back(GroupMerge{range.first, range.slot});
      s.subtracts.push_back(GroupMerge{range.end, range.slot});
    }
  }
  bool overflow = false;
  for (uint32_t i = 0; i < functions_.size(); ++i) {
    const AggregateFunction& function = *functions_[i];
    GroupStates states = States(s, i, false);
    function.Combine(s.merges.data(), static_cast<uint32_t>(s.merges.size()),
                     states, &overflow);
    function.Subtract(s.subtracts.data(),
                      static_cast<uint32_t>(s.subtracts.size()), states,
                      &overflow);
    function.Prefix(count, states, &overflow);
  }
  if (overflow) {
    return false;
  }
  const uint32_t* groups = GetTree().groups.data();
  for (uint32_t i = 0; i < functions_.size(); ++i) {
    s.outputs[i] = Finalized(*s.context, *functions_[i], States(s, i, false),
                             groups, count);
  }
  return true;
}

// A tree over the batch's segments, rounded up to a power of two `leaves`:
// the slot of each interval live in segments [first, end) is merged into the
// fewest nodes covering just those leaves, and each node is then pushed down
// into its children. A leaf then holds every interval live in its segment,
// whichever the function.
void IntervalFlatten::AggregateTree(State& s, bool* overflow) const {
  uint32_t count = s.segments;
  uint32_t leaves = base::RoundUpToPowerOfTwo(std::max(count, 1u));
  std::fill_n(s.states.data(), uint64_t{2 * leaves} * stride_, 0);
  // An interval is merged into at most two nodes a level. Written without
  // branches: whether a node is taken is unpredictable.
  uint32_t levels = static_cast<uint32_t>(base::CountTrailZeros(leaves)) + 1;
  s.merges.resize(uint64_t{s.ranges.size()} * 2 * levels);
  GroupMerge* out = s.merges.data();
  for (const Range& range : s.ranges) {
    for (uint32_t l = range.first + leaves, r = range.end + leaves; l < r;
         l /= 2, r /= 2) {
      uint32_t take_l = l & 1;
      *out = GroupMerge{l, range.slot};
      out += take_l;
      l += take_l;
      uint32_t take_r = r & 1;
      r -= take_r;
      *out = GroupMerge{r, range.slot};
      out += take_r;
    }
  }
  s.merges.resize(static_cast<uint64_t>(out - s.merges.data()));
  const Tree& tree = GetTree();
  for (uint32_t i = 0; i < functions_.size(); ++i) {
    const AggregateFunction& function = *functions_[i];
    GroupStates states = States(s, i, false);
    if (states.seen) {
      memset(states.seen, 0, 2 * leaves);
    }
    function.Combine(s.merges.data(), static_cast<uint32_t>(s.merges.size()),
                     states, overflow);
    function.Combine(tree.pushes.data(), 2 * leaves - 2, states, overflow);
    s.outputs[i] = Finalized(*s.context, function, states,
                             tree.groups.data() + leaves, count);
  }
}

OpResult IntervalFlatten::Finish(RowBatch& out, OperatorState& state) const {
  auto& s = static_cast<State&>(state);
  out.Reset();
  s.segments = 0;
  if (s.status.ok() && s.in_group &&
      Advance(s, std::numeric_limits<int64_t>::max())) {
    s.in_group = false;
  }
  return Yield(
      out, s, s.in_group ? OpResult::kHaveMoreOutput : OpResult::kNeedMoreInput,
      nullptr);
}

OpResult IntervalFlatten::Yield(RowBatch& out,
                                State& s,
                                OpResult result,
                                const RowBatch* in) const {
  if (!s.status.ok() || !Apply(s, in)) {
    return OpResult::kError;
  }
  uint32_t count = s.segments;
  if (count == 0) {
    return result;
  }
  // The batch takes the segments' buffers; the next batch's first segment
  // takes new ones.
  out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, s.segment_ts),
                std::move(s.ts_buffer));
  out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, s.segment_dur),
                std::move(s.dur_buffer));
  if (!spec_.key_columns.empty()) {
    const uint32_t* rows = s.segment_key_rows.data();
    s.key_rows.View(&s.served_keys, {rows, rows + count}, *s.context);
    for (uint32_t k = 0; k < s.served_keys.column_count(); ++k) {
      out.AddColumn(s.served_keys.column(k), s.served_keys.buffer(k));
    }
  }
  for (ColumnBuffer& buffer : s.outputs) {
    ColumnChunk& chunk = buffer.chunk();
    out.AddColumn(
        ColumnView::Reference(StorageType{Int64{}}, chunk.Values<int64_t>(),
                              &chunk.validity),
        std::move(buffer));
  }
  out.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, s.segment_groups),
                std::move(s.group_buffer));
  out.SetRowCount(count);
  return result;
}

void IntervalFlatten::State::Reset() {
  State& s = *this;
  s.status = base::OkStatus();
  s.input_row = 0;
  s.input_pending = false;
  s.first_key = 0;
  s.input_group = 0;
  s.output_group = 0;
  s.in_group = false;
  s.cursor = 0;
  s.input_ts = 0;
  s.live = 0;
  s.instants.clear();
  s.ends.clear();
  // The tree's nodes are zeroed as used.
  s.states.clear();
  s.states.resize(uint64_t{kFirstSlot} * s.stride);
  s.slots = 0;
  s.free_slots.clear();
  s.ranges.clear();
  s.rows.clear();
  s.row_slots.clear();
  for (SeenBytes& bytes : s.seen_bytes) {
    bytes.Clear();
  }
  s.key_rows.Clear();
  s.segments = 0;
  s.retained.Reset();
  s.saved_key.Reset();
  s.key_row = 0;
  s.served_keys.Reset();
  s.ts_buffer = ColumnBuffer();
  s.dur_buffer = ColumnBuffer();
  s.group_buffer = ColumnBuffer();
}

base::Status IntervalFlatten::status(const OperatorState& state) const {
  return static_cast<const State&>(state).status;
}

}  // namespace perfetto::trace_processor::core::exec
