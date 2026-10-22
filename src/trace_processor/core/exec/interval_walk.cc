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

#include "src/trace_processor/core/exec/interval_walk.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/interval_lanes.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr char kName[] = "INTERVAL WALK";
constexpr uint32_t kNoLane = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kNoEdge = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kMaxWalkDepth = 64;

struct WalkResult {
  Ts start;
  Ts end;
  uint32_t in_row;
  uint32_t edge_row;
};

}  // namespace

struct IntervalWalk::State : OperatorState {
  State(const IntervalWalkSpec& spec, Context& c)
      : OperatorState(ResetEachRun{}),
        context(&c),
        edges(spec.edges, {kName, "the edges"}),
        input(spec.input, {kName, "the input"}) {}
  ~State() override;
  void Reset() override;

  PERFETTO_ALWAYS_INLINE bool IsOnStack(uint32_t target_lane,
                                        uint32_t current_lane,
                                        uint32_t depth) const {
    if (target_lane == current_lane) {
      return true;
    }
    for (uint32_t i = 0; i < depth; ++i) {
      if (active_lanes[i] == target_lane) {
        return true;
      }
    }
    return false;
  }

  Context* context;
  std::unique_ptr<OperatorState> edges_state;
  base::Status status = base::OkStatus();
  bool edges_read = false;

  // Numbers lanes by key across input, edges PER, and edges TO.
  GroupTable lanes;
  CollectedLanes edges;
  // Indexed by edge row ID in `edges.rows()`: dense lane index in `edges` of
  // the target lane, or `kNoLane` if the edge is a leaf.
  std::vector<uint32_t> next_lane;

  LaneReader input;
  RowBatch input_projected;
  RowStore input_store;

  std::array<uint32_t, kMaxWalkDepth> active_lanes{};
  std::vector<WalkResult> results;
  size_t served = 0;
  bool serving = false;

  std::vector<uint32_t> in_rows;
  std::vector<uint32_t> edge_rows;
  RowBatch gathered_input;
  RowBatch gathered_edges;
};

IntervalWalk::State::~State() = default;

void IntervalWalk::State::Reset() {
  status = base::OkStatus();
  edges_read = false;
  lanes.Clear();
  edges.Clear();
  next_lane.clear();
  input.Reset();
  input_projected.Reset();
  input_store.Clear();
  results.clear();
  served = 0;
  serving = false;
}

IntervalWalk::IntervalWalk(IntervalWalkSpec spec) : spec_(std::move(spec)) {
  PERFETTO_CHECK(!spec_.input.key_columns.empty());
  PERFETTO_CHECK(spec_.edges.lanes.key_columns.size() ==
                 spec_.input.key_columns.size());
  PERFETTO_CHECK(spec_.next_key_columns.size() ==
                 spec_.input.key_columns.size());
  for (const IntervalWalkSpec::Output& output : spec_.outputs) {
    if (output.source == IntervalWalkSpec::Source::kEdge) {
      has_edge_outputs_ = true;
    }
  }
}

IntervalWalk::~IntervalWalk() = default;

std::unique_ptr<OperatorState> IntervalWalk::MakeState(Context& context) const {
  auto state = std::make_unique<State>(spec_, context);
  state->edges_state = spec_.edges.source->MakeState(context);
  return state;
}

base::Status IntervalWalk::status(const OperatorState& state) const {
  return state.Cast<const State>().status;
}

bool IntervalWalk::EnsureEdgesRead(State& s) const {
  if (!s.edges_read) {
    spec_.edges.source->Rewind(*s.edges_state);
    s.status = ReadEdges(s);
    s.edges_read = true;
  }
  return s.status.ok();
}

base::Status IntervalWalk::ReadEdges(State& s) const {
  RETURN_IF_ERROR(s.edges.Collect(*s.edges_state, &s.lanes));
  uint32_t total_rows = s.edges.rows().size();
  s.next_lane.assign(total_rows, kNoLane);
  RowBatch batch;
  for (uint32_t offset = 0; offset < total_rows;) {
    uint32_t count = s.edges.rows().View(&batch, offset, kMaxBatchRows);
    PERFETTO_DCHECK(count > 0);
    if (std::optional<uint32_t> bad = s.lanes.FindOrCreate(
            batch, spec_.next_key_columns, s.next_lane.data() + offset)) {
      return base::ErrStatus(
          "%s: the edges' TO column %u must hold one type, matching PER", kName,
          *bad + 1);
    }
    const auto key_count = static_cast<uint32_t>(spec_.next_key_columns.size());
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t group = s.next_lane[offset + i];
      bool has_null = false;
      for (uint32_t k = 0; k < key_count; ++k) {
        if (s.lanes.IsNull(group, k)) {
          has_null = true;
          break;
        }
      }
      if (has_null) {
        s.next_lane[offset + i] = kNoLane;
      } else if (std::optional<uint32_t> target = s.edges.Find(group)) {
        s.next_lane[offset + i] = *target;
      } else {
        s.next_lane[offset + i] = kNoLane;
      }
    }
    offset += count;
  }
  return base::OkStatus();
}

void IntervalWalk::WalkRange(Ts start,
                             Ts end,
                             uint32_t lane_idx,
                             uint32_t fallback_edge,
                             uint32_t in_row,
                             uint32_t depth,
                             State& s) const {
  const CollectedLanes::Lane lane = s.edges.lane(lane_idx);
  Span<const Interval> intervals = lane.intervals;
  const Interval* it =
      lane.nonoverlapping
          ? std::partition_point(
                intervals.begin(), intervals.end(),
                [start](const Interval& iv) { return iv.end <= start; })
          : intervals.begin();
  Ts pos = start;
  for (; it != intervals.end() && it->start < end; ++it) {
    if (it->end <= pos) {
      continue;
    }
    if (fallback_edge != kNoEdge && it->start > pos) {
      s.results.push_back({pos, it->start, in_row, fallback_edge});
    }
    Ts ov_start = std::max(pos, it->start);
    Ts ov_end = std::min(end, it->end);
    uint32_t edge_id = it->id;
    uint32_t next_lane = s.next_lane[edge_id];
    if (next_lane == kNoLane || depth >= kMaxWalkDepth ||
        s.IsOnStack(next_lane, lane_idx, depth)) {
      s.results.push_back({ov_start, ov_end, in_row, edge_id});
    } else {
      s.active_lanes[depth] = lane_idx;
      WalkRange(ov_start, ov_end, next_lane, edge_id, in_row, depth + 1, s);
    }
    pos = ov_end;
  }
  if (fallback_edge != kNoEdge && pos < end) {
    s.results.push_back({pos, end, in_row, fallback_edge});
  }
}

base::Status IntervalWalk::WalkBatch(const RowBatch& in, State& s) const {
  RETURN_IF_ERROR(s.input.Start(in, &s.lanes));
  if (!spec_.input_retained_columns.empty()) {
    s.input_store.Clear();
    s.input_projected.Project(in, spec_.input_retained_columns);
    RETURN_IF_ERROR(s.input_store.Append(s.input_projected));
  }
  LaneBatch lanes = s.input.batch();
  s.results.clear();
  for (uint32_t row = 0; row < in.size(); ++row) {
    int64_t start;
    int64_t end;
    LaneBatch::Result result = lanes.Read(row, &start, &end);
    if (result != LaneBatch::Result::kOk) {
      if (result == LaneBatch::Result::kSkip) {
        continue;
      }
      return lanes.Error(result, {kName, "the input"});
    }
    if (start == end) {
      continue;
    }
    std::optional<uint32_t> edge_lane = s.edges.Find(lanes.lane(row));
    if (!edge_lane) {
      continue;
    }
    WalkRange(static_cast<Ts>(start), static_cast<Ts>(end), *edge_lane, kNoEdge,
              row, 0, s);
  }
  return base::OkStatus();
}

OpResult IntervalWalk::Execute(const RowBatch& in,
                               RowBatch& out,
                               OperatorState& state) const {
  auto& s = state.Cast<State>();
  out.Reset();
  if (s.serving) {
    return ServeResults(out, s);
  }
  if (!EnsureEdgesRead(s)) {
    return OpResult::kError;
  }
  s.status = WalkBatch(in, s);
  if (!s.status.ok()) {
    return OpResult::kError;
  }
  if (s.results.empty()) {
    return OpResult::kNeedMoreInput;
  }
  s.served = 0;
  s.serving = true;
  return ServeResults(out, s);
}

OpResult IntervalWalk::ServeResults(RowBatch& out, State& s) const {
  auto count = static_cast<uint32_t>(
      std::min<size_t>(s.results.size() - s.served, kMaxBatchRows));
  const WalkResult* results = s.results.data() + s.served;

  BoundsColumns bounds;
  bounds.Take(*s.context);
  s.in_rows.resize(count);
  s.edge_rows.resize(count);
  PERFETTO_NO_VECTORIZE_LOOP
  for (uint32_t i = 0; i < count; ++i) {
    bounds.ts[i] = static_cast<int64_t>(results[i].start);
    bounds.dur[i] = static_cast<int64_t>(results[i].end - results[i].start);
    s.in_rows[i] = results[i].in_row;
    s.edge_rows[i] = results[i].edge_row;
  }
  if (!spec_.input_retained_columns.empty()) {
    const uint32_t* rows = s.in_rows.data();
    s.input_store.Gather(&s.gathered_input, {rows, rows + count}, *s.context);
  }
  if (has_edge_outputs_) {
    const uint32_t* rows = s.edge_rows.data();
    s.edges.rows().Gather(&s.gathered_edges, {rows, rows + count}, *s.context);
  }

  for (const IntervalWalkSpec::Output& output : spec_.outputs) {
    switch (output.source) {
      case IntervalWalkSpec::Source::kTs:
        bounds.AddTsTo(&out);
        break;
      case IntervalWalkSpec::Source::kDur:
        bounds.AddDurTo(&out);
        break;
      case IntervalWalkSpec::Source::kInput:
        out.AddColumn(s.gathered_input.column(output.index),
                      s.gathered_input.buffer(output.index));
        break;
      case IntervalWalkSpec::Source::kEdge:
        out.AddColumn(s.gathered_edges.column(output.index),
                      s.gathered_edges.buffer(output.index));
        break;
    }
  }
  out.SetRowCount(count);
  s.served += count;
  if (s.served < s.results.size()) {
    return OpResult::kHaveMoreOutput;
  }
  s.results.clear();
  s.served = 0;
  s.serving = false;
  return OpResult::kNeedMoreInput;
}

}  // namespace perfetto::trace_processor::core::exec
