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
#include "src/trace_processor/core/exec/interval_lanes.h"
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

class IntersectState : public OperatorState {
 public:
  ~IntersectState() override;

  std::vector<std::unique_ptr<OperatorState>> operand_states;
  // Shared by all operands, so a key has the same number in each of them.
  // This only works if their key types match.
  GroupTable group_table;
  // Per operand, its intervals by lane. A key with no lane in some operand
  // covers nothing, so only keys every operand has produce regions.
  std::vector<std::unique_ptr<CollectedLanes>> lanes;

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
    // Per operand, its lane of the key being narrowed.
    std::vector<CollectedLanes::Lane> key_lanes;
  } scratch;
};

IntersectState::~IntersectState() = default;

// Narrows the regions of one key. If every operand is non-overlapping, runs a
// fast k-pointer sweep. Otherwise, narrows through operands pairwise, smallest
// first so the running set starts as small as it can.
void NarrowKey(const std::vector<CollectedLanes::Lane>& groups,
               IntersectState::Scratch& scratch,
               Regions& out) {
  if (std::all_of(
          groups.begin(), groups.end(),
          [](const CollectedLanes::Lane& g) { return g.nonoverlapping; })) {
    base::SmallVector<const Span<const Interval>*, 16> tables;
    for (const CollectedLanes::Lane& group : groups) {
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
    const CollectedLanes::Lane& group = groups[which];
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
    state->lanes.push_back(std::make_unique<CollectedLanes>(
        operands_[i].lanes,
        IntervalInputName{"INTERVAL INTERSECTION", "operand", i + 1}));
  }
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
    s.lanes[i]->Clear();
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
      // A key no earlier operand has can produce no region, so is skipped.
      uint32_t limit = i == 0 || operands_[i].lanes.key_columns.empty()
                           ? std::numeric_limits<uint32_t>::max()
                           : s.group_table.size();
      s.status = s.lanes[i]->Collect(operands_[i], *s.operand_states[i],
                                     &s.group_table, limit);
      if (!s.status.ok()) {
        return false;
      }
    }
    // A region needs every operand, so only the keys of the operand with the
    // fewest of them can start one.
    uint32_t fewest = 0;
    for (uint32_t i = 1; i < count; ++i) {
      if (s.lanes[i]->size() < s.lanes[fewest]->size()) {
        fewest = i;
      }
    }
    std::vector<CollectedLanes::Lane>& lanes = s.scratch.key_lanes;
    lanes.resize(count);
    for (uint32_t l = 0; l < s.lanes[fewest]->size(); ++l) {
      uint32_t number = s.lanes[fewest]->lane(l).number;
      bool everywhere = true;
      for (uint32_t i = 0; i < count && everywhere; ++i) {
        std::optional<uint32_t> found = s.lanes[i]->Find(number);
        everywhere = found.has_value();
        if (everywhere) {
          lanes[i] = s.lanes[i]->lane(*found);
        }
      }
      if (everywhere) {
        NarrowKey(lanes, s.scratch, s.regions);
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

  BoundsColumns bounds;
  bounds.Take(*s.context);
  for (uint32_t i = 0; i < count; ++i) {
    s.scratch.gather_rows[i].resize(serving);
  }
  for (uint32_t row = 0; row < serving; ++row) {
    uint32_t at = s.served + row;
    bounds.ts[row] = static_cast<int64_t>(s.regions.starts[at]);
    bounds.dur[row] =
        static_cast<int64_t>(s.regions.ends[at] - s.regions.starts[at]);
    const uint32_t* from = s.regions.RowsOf(at);
    for (uint32_t i = 0; i < count; ++i) {
      s.scratch.gather_rows[i][row] = from[i];
    }
  }
  s.served += serving;

  out.Reset();
  bounds.AddTo(&out);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t* rows = s.scratch.gather_rows[i].data();
    s.lanes[i]->rows().View(&s.gathered[i], {rows, rows + serving}, *s.context);
    for (uint32_t c = 0; c < s.gathered[i].column_count(); ++c) {
      out.AddColumn(s.gathered[i].column(c), s.gathered[i].buffer(c));
    }
  }
  out.SetRowCount(serving);
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
