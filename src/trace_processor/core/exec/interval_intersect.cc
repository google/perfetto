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

#include "src/trace_processor/core/exec/interval_intersect.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/containers/interval_intersector.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"
#include "src/trace_processor/util/cold_sort.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Regions and the row each came from in every operand: the bounds in one
// array and the rows in another, `operands` to a region, so narrowing copies
// a run of indices rather than reaching for the heap once a region.
struct Regions {
  uint32_t operands = 0;
  std::vector<Ts> starts;
  std::vector<Ts> ends;
  std::vector<uint32_t> rows;

  uint32_t size() const { return static_cast<uint32_t>(starts.size()); }
  const uint32_t* RowsOf(uint32_t i) const {
    return rows.data() + static_cast<size_t>(i) * operands;
  }
  void Clear() {
    starts.clear();
    ends.clear();
    rows.clear();
  }
  // Adds a region covering [start, end) whose rows are `from`, which may be
  // null when none are known yet.
  uint32_t* Push(Ts start, Ts end, const uint32_t* from) {
    starts.push_back(start);
    ends.push_back(end);
    size_t at = rows.size();
    rows.resize(at + operands, 0);
    uint32_t* to = rows.data() + at;
    if (from) {
      std::copy(from, from + operands, to);
    }
    return to;
  }
};

// The intervals of one key in one operand.
struct Group {
  Span<const Interval> intervals;
  // Whether no two intervals overlap. If so, they can be searched without
  // building a tree.
  bool nonoverlapping;
};

// The intervals of one operand. They arrive grouped by key and sorted by
// start within each group, so they are kept in that order in one vector and
// each group is a range of it. Kept across runs to reuse memory.
struct Groups {
  struct Range {
    uint32_t begin;
    uint32_t end;
    bool nonoverlapping;
  };
  std::vector<Interval> intervals;
  std::vector<Range> ranges;
  // The shared group number of each range.
  std::vector<uint32_t> seen;
  // Each range's shared group number and index, sorted by number. Unlike an
  // array indexed by number, it grows with this operand's groups only.
  std::vector<std::pair<uint32_t, uint32_t>> by_number;
  // The operand's group number of the last row read.
  std::optional<uint32_t> last_group;

  // The shared group number of that row's key.
  uint32_t last_number = 0;

  void Add(uint32_t number, const Interval& interval) {
    if (seen.empty() || seen.back() != number) {
      auto at = static_cast<uint32_t>(intervals.size());
      ranges.push_back({at, at, true});
      seen.push_back(number);
    }
    Range& range = ranges.back();
    // Intervals arrive sorted by start, so comparing with the previous one
    // is enough to detect overlap.
    if (range.end > range.begin && IsOverlapping(intervals.back(), interval)) {
      range.nonoverlapping = false;
    }
    intervals.push_back(interval);
    range.end++;
  }
  // Called once every interval is added, before Find().
  void Index() {
    by_number.clear();
    for (uint32_t i = 0; i < seen.size(); ++i) {
      by_number.emplace_back(seen[i], i);
    }
    if (!std::is_sorted(by_number.begin(), by_number.end())) {
      std::sort(by_number.begin(), by_number.end());
    }
  }
  std::optional<Group> Find(uint32_t number) const {
    auto it = std::lower_bound(by_number.begin(), by_number.end(), number,
                               [](const std::pair<uint32_t, uint32_t>& p,
                                  uint32_t n) { return p.first < n; });
    if (it == by_number.end() || it->first != number) {
      return std::nullopt;
    }
    const Range& range = ranges[it->second];
    const Interval* data = intervals.data();
    return Group{{data + range.begin, data + range.end}, range.nonoverlapping};
  }
  void Clear() {
    intervals.clear();
    ranges.clear();
    seen.clear();
    by_number.clear();
    last_group.reset();
  }
};

class IntersectState : public OperatorState {
 public:
  ~IntersectState() override;

  std::vector<std::unique_ptr<OperatorState>> operand_states;
  std::vector<std::unique_ptr<RowStore>> stores;
  // Shared by all operands, so a key has the same number in each of them.
  // This only works if their key types match.
  GroupTable group_table;
  // Per operand, its rows by key. A key with no entry in some operand covers
  // nothing, so only keys every operand has produce regions.
  std::vector<Groups> groups;

  bool computed = false;
  Regions regions;
  uint32_t served = 0;

  // What the bounds of the regions served are written in, which no operand
  // owns.
  Context* context = nullptr;
  // One gathered batch per operand.
  std::vector<RowBatch> gathered;
  base::Status status = base::OkStatus();

  // Buffers reused across calls to avoid allocating: nothing in them outlives
  // the call which fills them.
  struct Scratch {
    // The regions of one key, before and after narrowing by an operand.
    Regions running;
    Regions narrowed;
    // The order in which a key's operands narrow its regions.
    std::vector<uint32_t> order;
    // The intervals of one operand overlapping a region.
    std::vector<Interval> overlaps;
    // Per operand, the row indices a gather reads.
    std::vector<std::vector<uint32_t>> gather_rows;
    // The current batch of an operand.
    RowBatch batch;

    // The part of the current batch that is kept.
    RowBatch retained;

    // The first row of each group in the current batch.
    RowBatch firsts;
    std::vector<uint32_t> first_rows;

    // Per operand, its group of the key being narrowed.
    std::vector<Group> key_groups;
    // The shared group number of each row of the current batch.
    std::vector<uint32_t> row_groups;

    // The shared group number of each first row.
    std::vector<uint32_t> first_numbers;
  } scratch;
};

IntersectState::~IntersectState() = default;

// Reads every row of `operand` into `store` and groups them by key.
base::Status Collect(const IntervalIntersectOperand& operand,
                     uint32_t which,
                     OperatorState& state,
                     RowStore& store,
                     GroupTable& group_table,
                     Groups& groups,
                     IntersectState::Scratch& scratch) {
  RowBatch& batch = scratch.batch;
  RowBatch& retained = scratch.retained;
  // A key no earlier operand has can produce no region, so is skipped.
  uint32_t known = which == 0 || operand.key_columns.empty()
                       ? std::numeric_limits<uint32_t>::max()
                       : group_table.size();
  while (operand.source->GetData(batch, state)) {
    PERFETTO_DCHECK(IsFlat<Int64>(batch.column(operand.ts_column)));
    PERFETTO_DCHECK(IsFlat<Int64>(batch.column(operand.dur_column)));
    FlatColumnReader<int64_t> ts(batch, operand.ts_column);
    FlatColumnReader<int64_t> dur(batch, operand.dur_column);
    // A group's rows arrive together, so only the first row of each group is
    // looked up in the shared group table.
    scratch.row_groups.assign(batch.size(), 0);
    if (!operand.key_columns.empty()) {
      scratch.first_rows.clear();
      std::optional<uint32_t> last = groups.last_group;
      for (uint32_t row = 0; row < batch.size(); ++row) {
        uint32_t group = batch.Value<uint32_t>(operand.group_column, row);
        if (group != last) {
          scratch.first_rows.push_back(row);
          last = group;
        }
      }
      scratch.firsts.CopyFrom(batch);
      const uint32_t* firsts = scratch.first_rows.data();
      scratch.firsts.mutable_selection().Keep(
          {firsts, firsts + scratch.first_rows.size()});
      std::vector<uint32_t>& numbers = scratch.first_numbers;
      numbers.resize(scratch.first_rows.size());
      if (std::optional<uint32_t> bad = group_table.FindOrCreate(
              scratch.firsts, operand.key_columns, numbers.data())) {
        return base::ErrStatus(
            "INTERVAL INTERSECTION: operand %u's PER column %u must hold one "
            "type, the same in every operand",
            which + 1, *bad + 1);
      }
      size_t next = 0;
      for (uint32_t row = 0; row < batch.size(); ++row) {
        if (next < scratch.first_rows.size() &&
            scratch.first_rows[next] == row) {
          groups.last_number = numbers[next++];
        }
        scratch.row_groups[row] = groups.last_number;
      }
      groups.last_group = last;
    }
    for (uint32_t row = 0; row < batch.size(); ++row) {
      int64_t start;
      int64_t length;
      if (!ts.Read(row, &start) || !dur.Read(row, &length)) {
        continue;
      }
      if (start < 0 || length < 0) {
        return base::ErrStatus(
            "INTERVAL INTERSECTION: operand %u has a row whose ts or dur is "
            "below zero",
            which + 1);
      }
      int64_t end;
      if (!base::CheckedAdd(start, length, &end)) {
        return base::ErrStatus("INTERVAL INTERSECTION: integer overflow");
      }
      if (scratch.row_groups[row] >= known) {
        continue;
      }
      groups.Add(scratch.row_groups[row],
                 Interval{static_cast<Ts>(start), static_cast<Ts>(end),
                          store.size() + row});
    }
    retained.Project(batch, operand.retained_columns);
    RETURN_IF_ERROR(store.Append(retained));
  }
  RETURN_IF_ERROR(operand.source->status(state));

  return base::OkStatus();
}

// Narrows the regions of one key. If every operand is non-overlapping, runs a
// fast k-pointer sweep. Otherwise, narrows through operands pairwise, smallest
// first so the running set starts as small as it can.
void NarrowKey(const std::vector<Group>& groups,
               IntersectState::Scratch& scratch,
               Regions& out) {
  if (std::all_of(groups.begin(), groups.end(),
                  [](const Group& g) { return g.nonoverlapping; })) {
    base::SmallVector<const Span<const Interval>*, 16> tables;
    for (const Group& group : groups) {
      tables.emplace_back(&group.intervals);
    }
    IntervalIntersector::IntersectNonOverlapping(
        tables, [&](uint64_t start, uint64_t end, const uint32_t* ids) {
          out.Push(start, end, ids);
        });
    return;
  }

  Regions& running = scratch.running;
  Regions& narrowed = scratch.narrowed;
  std::vector<uint32_t>& order = scratch.order;
  std::vector<Interval>& overlaps = scratch.overlaps;

  auto count = static_cast<uint32_t>(groups.size());
  order.resize(count);
  std::iota(order.begin(), order.end(), 0u);
  ColdSortByKey(order.begin(), order.end(),
                [&](uint32_t i) { return groups[i].intervals.size(); });

  running.Clear();
  for (const Interval& interval : groups[order.front()].intervals) {
    running.Push(interval.start, interval.end, nullptr)[order.front()] =
        interval.id;
  }

  for (uint32_t i = 1; i < count && running.size() > 0; ++i) {
    uint32_t which = order[i];
    const Group& group = groups[which];
    IntervalIntersector intersector(
        group.intervals.data(), group.intervals.size(),
        IntervalIntersector::DecideMode(group.nonoverlapping, running.size()));
    narrowed.Clear();
    for (uint32_t r = 0; r < running.size(); ++r) {
      overlaps.clear();
      intersector.FindOverlaps(running.starts[r], running.ends[r], overlaps);
      const uint32_t* from = running.RowsOf(r);
      for (const Interval& overlap : overlaps) {
        narrowed.Push(overlap.start, overlap.end, from)[which] = overlap.id;
      }
    }
    std::swap(running, narrowed);
  }
  for (uint32_t r = 0; r < running.size(); ++r) {
    out.Push(running.starts[r], running.ends[r], running.RowsOf(r));
  }
}

}  // namespace

IntervalIntersect::IntervalIntersect(
    std::vector<IntervalIntersectOperand> operands)
    : operands_(std::move(operands)) {
  PERFETTO_CHECK(operands_.size() >= 2);
}

IntervalIntersect::~IntervalIntersect() = default;

std::unique_ptr<OperatorState> IntervalIntersect::MakeState(
    Context& context) const {
  auto state = std::make_unique<IntersectState>();
  state->context = &context;
  auto count = static_cast<uint32_t>(operands_.size());
  for (uint32_t i = 0; i < count; ++i) {
    state->stores.push_back(std::make_unique<RowStore>());
  }
  state->groups.resize(count);
  state->gathered.resize(count);
  state->scratch.gather_rows.resize(count);
  for (const IntervalIntersectOperand& operand : operands_) {
    state->operand_states.push_back(operand.source->MakeState(context));
  }
  return state;
}

base::Status IntervalIntersect::status(const OperatorState& state) const {
  return state.Cast<const IntersectState>().status;
}

void IntervalIntersect::Rewind(OperatorState& state) const {
  IntersectState& s = state.Cast<IntersectState>();
  for (uint32_t i = 0; i < operands_.size(); ++i) {
    operands_[i].source->Rewind(*s.operand_states[i]);
    s.stores[i]->Clear();
    s.groups[i].Clear();
  }
  s.group_table.Clear();
  s.computed = false;
  s.regions.Clear();
  s.served = 0;
  s.status = base::OkStatus();
}

bool IntervalIntersect::GetData(RowBatch& out, OperatorState& state) const {
  IntersectState& s = state.Cast<IntersectState>();
  auto count = static_cast<uint32_t>(operands_.size());
  if (!s.computed) {
    s.regions.operands = count;
    s.scratch.running.operands = count;
    s.scratch.narrowed.operands = count;
    for (uint32_t i = 0; i < count; ++i) {
      s.status = Collect(operands_[i], i, *s.operand_states[i], *s.stores[i],
                         s.group_table, s.groups[i], s.scratch);
      if (!s.status.ok()) {
        return false;
      }
      s.groups[i].Index();
    }
    // A region needs every operand, so only the keys of the operand with the
    // fewest of them can start one.
    uint32_t fewest = 0;
    for (uint32_t i = 1; i < count; ++i) {
      if (s.groups[i].seen.size() < s.groups[fewest].seen.size()) {
        fewest = i;
      }
    }
    std::vector<Group>& groups = s.scratch.key_groups;
    groups.resize(count);
    for (uint32_t number : s.groups[fewest].seen) {
      bool everywhere = true;
      for (uint32_t i = 0; i < count && everywhere; ++i) {
        std::optional<Group> group = s.groups[i].Find(number);
        everywhere = group.has_value();
        if (everywhere) {
          groups[i] = *group;
        }
      }
      if (everywhere) {
        NarrowKey(groups, s.scratch, s.regions);
      }
    }
    s.computed = true;
  }

  uint32_t pending = s.regions.size() - s.served;
  uint32_t serving = std::min(pending, kMaxBatchRows);
  if (serving == 0) {
    out.Reset();
    return false;
  }

  ColumnBuffer ts_buffer = s.context->TakeBuffer();
  ColumnBuffer dur_buffer = s.context->TakeBuffer();
  int64_t* ts = ts_buffer.chunk().Values<int64_t>();
  int64_t* dur = dur_buffer.chunk().Values<int64_t>();
  for (uint32_t i = 0; i < count; ++i) {
    s.scratch.gather_rows[i].resize(serving);
  }
  for (uint32_t row = 0; row < serving; ++row) {
    uint32_t at = s.served + row;
    ts[row] = static_cast<int64_t>(s.regions.starts[at]);
    dur[row] = static_cast<int64_t>(s.regions.ends[at] - s.regions.starts[at]);
    const uint32_t* from = s.regions.RowsOf(at);
    for (uint32_t i = 0; i < count; ++i) {
      s.scratch.gather_rows[i][row] = from[i];
    }
  }
  s.served += serving;

  out.Reset();
  out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, ts),
                std::move(ts_buffer));
  out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, dur),
                std::move(dur_buffer));
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t* rows = s.scratch.gather_rows[i].data();
    s.stores[i]->View(&s.gathered[i], {rows, rows + serving}, *s.context);
    for (uint32_t c = 0; c < s.gathered[i].column_count(); ++c) {
      out.AddColumn(s.gathered[i].column(c), s.gathered[i].buffer(c));
    }
  }
  out.SetRowCount(serving);
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
