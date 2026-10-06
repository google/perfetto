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
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/util/heap.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

bool IsInt64(const RowBatch& batch, uint32_t index) {
  const ColumnView& column = batch.column(index);
  return column.kind() == ColumnView::Kind::kFlat && column.type().Is<Int64>();
}

constexpr auto kEndsLater = [](const auto& a, const auto& b) {
  return a.end > b.end;
};

template <typename Live>
PERFETTO_ALWAYS_INLINE void PushEnd(FlexVector<Live>& heap, Live live) {
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

}  // namespace

IntervalFlatten::IntervalFlatten(IntervalFlattenSpec spec)
    : Operator({BatchPreference::kThroughput}),
      spec_(std::move(spec)),
      aggregation_(spec_.aggregates),
      sums_(aggregation_.sums()) {}

IntervalFlatten::~IntervalFlatten() = default;
IntervalFlatten::State::~State() = default;

std::unique_ptr<OperatorState> IntervalFlatten::MakeState() const {
  auto state = std::make_unique<State>();
  state->live = aggregation_.MakeTotals();
  state->instant = state->live;
  state->segment_aggregates = aggregation_.MakeResults();
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
  if (!IsInt64(in, spec_.ts_column) || !IsInt64(in, spec_.dur_column) ||
      !aggregation_.CanRead(in)) {
    s.status = base::ErrStatus(
        "INTERVAL FLATTEN: ts, dur and summed columns must be Int64");
    return OpResult::kError;
  }
  FlatColumnReader<int64_t> ts(in.column(spec_.ts_column));
  FlatColumnReader<int64_t> dur(in.column(spec_.dur_column));
  Aggregation::Readers sums;
  aggregation_.Read(in, &sums);
  // A full output batch can interrupt consumption of the same input. Keep its
  // keys in place until all rows have been consumed.
  if (!s.input_pending) {
    if (!RetainKeys(in, s)) {
      return OpResult::kError;
    }
    s.input_pending = true;
  }
  bool keyed = !spec_.key_columns.empty();
  const ColumnView* groups = keyed ? &in.column(spec_.group_column) : nullptr;
  uint32_t rows = in.size();
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
    uint32_t group = keyed ? groups->Value<uint32_t>(row) : 0;
    if (!s.in_group || group != s.input_group) {
      if (s.in_group) {
        // Finish the old group before accepting any rows from the new one.
        // Several small groups can still share one output batch.
        if (!Advance(s, std::numeric_limits<int64_t>::max())) {
          return Yield(out, s, OpResult::kHaveMoreOutput);
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
        return Yield(out, s, OpResult::kHaveMoreOutput);
      }
      s.input_ts = start;
    }
    if (!AddInterval(s, start, length,
                     {sums.data(), sums.data() + sums.size()})) {
      return OpResult::kError;
    }
  }
  s.input_row = 0;
  s.input_pending = false;
  // Only the current group's key must survive reuse of the input buffers.
  if (keyed && s.in_group) {
    s.key_rows.View(&s.saved_key, s.key_row, 1);
  }
  return Yield(out, s, OpResult::kNeedMoreInput);
}

PERFETTO_ALWAYS_INLINE bool IntervalFlatten::AddInterval(
    State& s,
    int64_t start,
    int64_t length,
    Span<const FlatColumnReader<int64_t>> sums) const {
  Totals* totals = &s.instant;
  uint32_t slot = 0;
  if (length > 0) {
    totals = &s.live;
    if (s.free_slots.empty()) {
      slot = s.slots++;
      s.slot_sums.resize(static_cast<uint64_t>(s.slots) * sums_);
    } else {
      slot = s.free_slots.back();
      s.free_slots.pop_back();
    }
    int64_t end;
    if (!Add(s, start, length, &end)) {
      return false;
    }
    PushEnd(s.ends, Live{end, slot});
  }
  ++totals->count;
  for (uint32_t i = 0; i < sums_; ++i) {
    Sum held = Sum::Of(sums[i], s.input_row);
    if (!totals->sums[i].Add(held)) {
      return Overflow(s);
    }
    if (length > 0) {
      s.slot_sums[static_cast<size_t>(slot) * sums_ + i] = held;
    }
  }
  return true;
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
  s.retained.Reset();
  for (uint32_t column : spec_.key_columns) {
    s.retained.AddColumn(in.column(column), in.owner(column));
  }
  s.retained.SetCardinality(in.size());
  s.status = s.key_rows.Append(s.retained);
  return s.status.ok();
}

PERFETTO_ALWAYS_INLINE bool IntervalFlatten::Add(State& s,
                                                 int64_t a,
                                                 int64_t b,
                                                 int64_t* out) {
  return base::CheckedAdd(a, b, out) || Overflow(s);
}

bool IntervalFlatten::Overflow(State& s) {
  s.status = base::ErrStatus("INTERVAL FLATTEN: integer overflow");
  return false;
}

bool IntervalFlatten::Advance(State& s, int64_t time) const {
  // Points at the last input timestamp include all rows starting there, so
  // emit them only once that timestamp is complete.
  if (s.instant.count > 0) {
    if (!Emit(s, s.input_ts, 0, true)) {
      return false;
    }
    s.instant.Clear();
  }
  int64_t& cursor = s.cursor;
  while (!s.ends.empty() && s.ends.begin()->end <= time) {
    int64_t end = s.ends.begin()->end;
    if (s.live.count > 0 && cursor < end) {
      if (!Emit(s, cursor, end - cursor, false)) {
        return false;
      }
    }
    cursor = end;
    if (!Expire(s, end)) {
      return false;
    }
  }
  if (s.live.count > 0 && cursor < time) {
    if (!Emit(s, cursor, time - cursor, false)) {
      return false;
    }
  }
  s.cursor = time;
  return true;
}

PERFETTO_ALWAYS_INLINE bool IntervalFlatten::Expire(State& s,
                                                    int64_t end) const {
  while (!s.ends.empty() && s.ends.begin()->end == end) {
    uint32_t slot = s.ends.begin()->slot;
    PopEnd(s.ends);
    --s.live.count;
    for (uint32_t i = 0; i < sums_; ++i) {
      const Sum& held = s.slot_sums[static_cast<size_t>(slot) * sums_ + i];
      if (!s.live.sums[i].Subtract(held)) {
        return Overflow(s);
      }
    }
    s.free_slots.push_back(slot);
  }
  return true;
}

bool IntervalFlatten::Emit(State& s,
                           int64_t ts,
                           int64_t dur,
                           bool with_instant) const {
  // A full batch leaves the pending segment untouched. Advance can resume
  // by retrying it, without tracking a partially processed boundary.
  uint32_t n = s.segments;
  if (n == kMaxBatchRows) {
    return false;
  }
  if (PERFETTO_UNLIKELY(n == s.segment_capacity)) {
    GrowSegments(s);
  }
  s.segment_ts[n] = ts;
  s.segment_dur[n] = dur;
  s.segment_groups[n] = s.output_group;
  s.segment_key_rows[n] = s.key_row;
  const Totals& live = s.live;
  const Totals& instant = s.instant;
  s.segment_aggregates.SetCount(
      n, live.count + (with_instant ? instant.count : 0));
  for (uint32_t i = 0; i < sums_; ++i) {
    Sum total = live.sums[i];
    if (with_instant && !total.Add(instant.sums[i])) {
      return Overflow(s);
    }
    s.segment_aggregates.SetSum(n, i, total);
  }
  s.segments = n + 1;
  return true;
}

PERFETTO_NO_INLINE void IntervalFlatten::GrowSegments(State& s) const {
  s.segment_capacity = std::max(64u, s.segment_capacity * 2);
  s.segment_ts.resize(s.segment_capacity);
  s.segment_dur.resize(s.segment_capacity);
  s.segment_groups.resize(s.segment_capacity);
  s.segment_key_rows.resize(s.segment_capacity);
  s.segment_aggregates.Resize(s.segment_capacity);
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
      out, s,
      s.in_group ? OpResult::kHaveMoreOutput : OpResult::kNeedMoreInput);
}

OpResult IntervalFlatten::Yield(RowBatch& out,
                                State& s,
                                OpResult result) const {
  if (!s.status.ok()) {
    return OpResult::kError;
  }
  uint32_t count = s.segments;
  if (count == 0) {
    return result;
  }
  auto add = [&](StorageType type, const void* values,
                 const BitVector* validity) {
    ColumnView view = ColumnView::Reference(type, values, validity);
    out.AddColumn(view);
  };
  add(StorageType{Int64{}}, s.segment_ts.data(), nullptr);
  add(StorageType{Int64{}}, s.segment_dur.data(), nullptr);
  if (!spec_.key_columns.empty()) {
    const uint32_t* rows = s.segment_key_rows.data();
    s.key_rows.View(&s.served_keys, {rows, rows + count});
    for (uint32_t k = 0; k < s.served_keys.column_count(); ++k) {
      out.AddColumn(s.served_keys.column(k), s.served_keys.owner(k));
    }
  }
  aggregation_.AddColumns(s.segment_aggregates, &out);
  add(StorageType{Uint32{}}, s.segment_groups.data(), nullptr);
  out.SetCardinality(count);
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
  s.live.Clear();
  s.instant.Clear();
  s.ends.clear();
  s.slots = 0;
  s.slot_sums.clear();
  s.free_slots.clear();
  s.key_rows.Clear();
  s.segments = 0;
  s.retained.Reset();
  s.saved_key.Reset();
  s.key_row = 0;
  s.served_keys.Reset();
}

base::Status IntervalFlatten::status(const OperatorState& state) const {
  return static_cast<const State&>(state).status;
}

}  // namespace perfetto::trace_processor::core::exec
