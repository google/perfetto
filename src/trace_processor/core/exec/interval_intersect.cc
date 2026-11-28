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
#include <limits>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/containers/interval_intersector.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/interval_lanes.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/run_vector.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/sink.h"
#include "src/trace_processor/core/util/arena.h"
#include "src/trace_processor/util/sort.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Regions and the row each came from in every operand: the bounds in one
// array and the rows in another, `operands` to a region, so narrowing copies
// a run of indices rather than reaching for the heap once a region.
struct Regions {
  uint32_t operands = 0;
  RunVector<Ts> starts;
  RunVector<Ts> ends;
  RunVector<uint32_t> rows;

  uint32_t size() const { return starts.size(); }
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
  uint32_t* Push(Ts start, Ts end, const uint32_t* from, OperatorState& owner) {
    starts.push_back(owner, start);
    ends.push_back(owner, end);
    uint32_t at = rows.size();
    rows.push_back_multiple(owner, 0, operands);
    uint32_t* to = rows.data() + at;
    if (from) {
      std::copy(from, from + operands, to);
    }
    return to;
  }
};

struct IntersectState : OperatorState {
  // Each operand, streamed in through the run's Context.
  core::ArenaSpan<PipelineUse> operand_inputs;

  // The operands' intervals, read in full.
  // Shared by all operands, so a key has the same number in each of them.
  // This only works if their key types match.
  GroupTable group_table;
  // Per operand, its intervals by lane. A key with no lane in some operand
  // covers nothing, so only keys every operand has produce regions.
  core::ArenaSpan<CollectedLanes> lanes;

  // The regions, computed once and then served.
  bool computed = false;
  Regions regions;
  uint32_t served = 0;
  // One gathered batch per operand.
  core::ArenaSpan<RowBatch*> gathered;
  BoundsColumns bounds;

  // Memory reused across calls: nothing in it outlives the call which fills
  // it.
  struct Scratch {
    // The regions of one key, before and after narrowing by an operand.
    Regions running;
    Regions narrowed;
    // The order in which a key's operands narrow its regions.
    core::ArenaSpan<uint32_t> order;
    // The intervals of one operand overlapping a region, in a vector as
    // IntervalIntersector fills it.
    std::vector<Interval>* overlaps = nullptr;
    // Per operand, kMaxBatchRows row indices for a gather to read.
    core::ArenaSpan<uint32_t> gather_rows;
    // Per operand, its lane of the key being narrowed.
    core::ArenaSpan<CollectedLanes::Lane> key_lanes;
  };
  Scratch scratch;
};

// Narrows the regions of one key. If every operand is non-overlapping, runs a
// fast k-pointer sweep. Otherwise, narrows through operands pairwise, smallest
// first so the running set starts as small as it can.
void NarrowKey(base::Span<const CollectedLanes::Lane> groups,
               IntersectState& s) {
  IntersectState::Scratch& scratch = s.scratch;
  Regions& out = s.regions;
  if (std::all_of(
          groups.begin(), groups.end(),
          [](const CollectedLanes::Lane& g) { return g.nonoverlapping; })) {
    base::SmallVector<const base::Span<const Interval>*, 16> tables;
    for (const CollectedLanes::Lane& group : groups) {
      tables.emplace_back(&group.intervals);
    }
    IntervalIntersector::IntersectNonOverlapping(
        tables, [&](uint64_t start, uint64_t end, const uint32_t* ids) {
          out.Push(start, end, ids, s);
        });
    return;
  }

  Regions& running = scratch.running;
  Regions& narrowed = scratch.narrowed;
  uint32_t* order = scratch.order.begin();
  std::vector<Interval>& overlaps = *scratch.overlaps;

  auto count = static_cast<uint32_t>(groups.size());
  std::iota(order, order + count, 0u);
  SortByKey(order, order + count,
            [&](uint32_t i) { return groups[i].intervals.size(); });

  running.Clear();
  for (const Interval& interval : groups[order[0]].intervals) {
    running.Push(interval.start, interval.end, nullptr, s)[order[0]] =
        interval.id;
  }

  for (uint32_t i = 1; i < count && running.size() > 0; ++i) {
    uint32_t which = order[i];
    const CollectedLanes::Lane& group = groups[which];
    IntervalIntersector intersector(
        group.intervals.data(), group.intervals.size(),
        IntervalIntersector::DecideMode(group.nonoverlapping, running.size()));
    narrowed.Clear();
    uint32_t n = running.size();
    const Ts* starts = running.starts.data();
    const Ts* ends = running.ends.data();
    const uint32_t* from = running.RowsOf(0);
    for (uint32_t r = 0; r < n; ++r, from += count) {
      overlaps.clear();
      intersector.FindOverlaps(starts[r], ends[r], overlaps);
      for (const Interval& overlap : overlaps) {
        narrowed.Push(overlap.start, overlap.end, from, s)[which] = overlap.id;
      }
    }
    std::swap(running, narrowed);
  }
  uint32_t n = running.size();
  const Ts* starts = running.starts.data();
  const Ts* ends = running.ends.data();
  const uint32_t* from = running.RowsOf(0);
  for (uint32_t r = 0; r < n; ++r, from += count) {
    out.Push(starts[r], ends[r], from, s);
  }
}

}  // namespace

IntervalIntersect::IntervalIntersect(
    core::ArenaSpan<CollectedInputSpec> operands)
    : operands_(operands) {
  PERFETTO_CHECK(operands_.size() >= 2);
}

OperatorState* IntervalIntersect::NewState(Context& context) const {
  auto* state = context.Make<IntersectState>();
  uint32_t count = operands_.size();
  state->operand_inputs = context.NewSpan<PipelineUse>(count);
  state->lanes = context.NewRunSpan<CollectedLanes>(*state, count);
  state->gathered = context.NewSpan<RowBatch*>(count);
  for (uint32_t i = 0; i < count; ++i) {
    state->operand_inputs[i] = PipelineUse(*state, operands_[i].pipeline);
    state->lanes[i].Init(
        operands_[i],
        IntervalInputName{"INTERVAL INTERSECTION", "operand", i + 1}, *state);
    state->gathered[i] = context.NewBatch(*state);
  }
  state->regions.operands = count;
  state->bounds.Init(*state);
  IntersectState::Scratch& scratch = state->scratch;
  scratch.running.operands = count;
  scratch.narrowed.operands = count;
  scratch.order = context.NewSpan<uint32_t>(count);
  scratch.overlaps = context.Own<std::vector<Interval>>();
  scratch.gather_rows = context.NewSpan<uint32_t>(count * kMaxBatchRows);
  scratch.key_lanes = context.NewSpan<CollectedLanes::Lane>(count);
  return state;
}

void IntervalIntersect::Rewind(OperatorState&) const {}

bool IntervalIntersect::GetData(RowBatch& out, OperatorState& state) const {
  IntersectState& s = state.Cast<IntersectState>();
  auto count = static_cast<uint32_t>(operands_.size());
  if (!s.computed) {
    for (uint32_t i = 0; i < count; ++i) {
      // A key no earlier operand has can produce no region, so is skipped.
      uint32_t limit = i == 0 || operands_[i].lanes.key_columns.empty()
                           ? std::numeric_limits<uint32_t>::max()
                           : s.group_table.size();
      base::Status status =
          s.lanes[i].Collect(s.operand_inputs[i], s, &s.group_table, limit);
      if (!status.ok()) {
        s.context().Fail(std::move(status));
        return false;
      }
    }
    // A region needs every operand, so only the keys of the operand with the
    // fewest of them can start one.
    uint32_t fewest = 0;
    for (uint32_t i = 1; i < count; ++i) {
      if (s.lanes[i].size() < s.lanes[fewest].size()) {
        fewest = i;
      }
    }
    core::ArenaSpan<CollectedLanes::Lane> lanes = s.scratch.key_lanes;
    const CollectedLanes& smallest = s.lanes[fewest];
    for (uint32_t l = 0, n = smallest.size(); l < n; ++l) {
      uint32_t number = smallest.lane(l).number;
      bool everywhere = true;
      for (uint32_t i = 0; i < count && everywhere; ++i) {
        if (i == fewest) {
          lanes[i] = s.lanes[i].lane(l);
          continue;
        }
        std::optional<uint32_t> found = s.lanes[i].Find(number);
        everywhere = found.has_value();
        if (everywhere) {
          lanes[i] = s.lanes[i].lane(*found);
        }
      }
      if (everywhere) {
        NarrowKey(lanes, s);
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

  BoundsColumns& bounds = s.bounds;
  bounds.Take(s.context());
  uint32_t* gather_rows = s.scratch.gather_rows.begin();
  uint32_t served = s.served;
  const Ts* starts = s.regions.starts.data() + served;
  const Ts* ends = s.regions.ends.data() + served;
  const uint32_t* from = s.regions.RowsOf(served);
  for (uint32_t row = 0; row < serving; ++row, from += count) {
    bounds.ts[row] = static_cast<int64_t>(starts[row]);
    bounds.dur[row] = static_cast<int64_t>(ends[row] - starts[row]);
    for (uint32_t i = 0; i < count; ++i) {
      gather_rows[i * kMaxBatchRows + row] = from[i];
    }
  }
  s.served += serving;

  out.Reset();
  bounds.AddTo(&out);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t* rows = gather_rows + i * kMaxBatchRows;
    RowBatch& gathered = *s.gathered[i];
    s.lanes[i].rows().Gather(&gathered, {rows, rows + serving}, s);
    for (uint32_t c = 0; c < gathered.column_count(); ++c) {
      out.AddColumn(gathered.column(c), gathered.buffer(c));
    }
  }
  out.SetRowCount(serving);
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
