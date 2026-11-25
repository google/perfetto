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

#include "src/trace_processor/core/exec/interval_fill_gaps.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/interval_lanes.h"
#include "src/trace_processor/core/exec/run_vector.h"
#include "src/trace_processor/core/exec/sink.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/arena.h"
#include "src/trace_processor/core/util/interval_gaps.h"

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr char kName[] = "INTERVAL FILL GAPS";

}  // namespace

struct IntervalFillGaps::State : OperatorState {
  // The background, streamed in through the run's Context.
  PipelineUse background_input;

  // Numbers lanes by key on both sides, if both have PER columns.
  GroupTable lanes;

  // The background, read in full before any input.
  bool background_read = false;
  CollectedLanes background;
  // Whether each background lane had any input.
  RunVector<bool> visited;

  // The input, a lane at a time.
  bool finished = false;
  LaneReader input;
  // If broadcasting, the PER values of the input batch's lanes, and the
  // handle of the current lane's.
  LaneKeys keys;
  uint32_t lane_key = 0;
  // The current input lane.
  uint32_t lane = 0;
  IntervalGaps gaps;
  // The gaps found, in the order found.
  RunVector<IntervalGaps::Gap> found;

  // The fillers served.
  // The shape of each output's input column, from the first input batch:
  // empty until then.
  RunVector<ColumnShape> shapes;
  // How many gaps have been output, and whether the current input batch has
  // been output but its gaps have not.
  uint32_t served = 0;
  bool serving = false;
  RowBatch* gathered = nullptr;
  RowBatch* gathered_keys = nullptr;
  BoundsColumns bounds;

  // Scratch for one call: the rows of a filler being gathered.
  core::ArenaSpan<uint32_t> rows;
};

namespace {

// Stores each gap which a state's IntervalGaps finds.
struct KeepGap {
  void operator()(const IntervalGaps::Gap& gap) const {
    gaps->push_back(*owner, gap);
  }
  RunVector<IntervalGaps::Gap>* gaps;
  OperatorState* owner;
};

}  // namespace

IntervalFillGaps::IntervalFillGaps(core::Arena& arena,
                                   IntervalFillGapsSpec spec)
    : spec_(spec),
      input_columns_(arena.NewSpan<uint32_t>(spec.outputs.size())),
      filler_columns_(arena.NewSpan<FillerColumn>(spec.outputs.size())) {
  const auto& keys = spec_.input.key_columns;
  const auto& background_keys = spec_.background.lanes.key_columns;
  PERFETTO_CHECK(background_keys.empty() ||
                 background_keys.size() == keys.size());
  broadcast_ = !keys.empty() && background_keys.empty();
  shared_ = !keys.empty() && !broadcast_;
  for (uint32_t i = 0; i < spec_.outputs.size(); ++i) {
    const IntervalFillGapsSpec::Output& output = spec_.outputs[i];
    input_columns_[i] = output.input;
    // ts and dur always come from the filler, not the background.
    auto key = std::find(keys.begin(), keys.end(), output.input);
    if (output.input == spec_.input.ts_column) {
      filler_columns_[i] = {FillerSource::kTs, 0};
    } else if (output.input == spec_.input.dur_column) {
      filler_columns_[i] = {FillerSource::kDur, 0};
    } else if (output.background != IntervalFillGapsSpec::Output::kNone) {
      filler_columns_[i] = {FillerSource::kBackground, output.background};
    } else if (broadcast_ && key != keys.end()) {
      filler_columns_[i] = {FillerSource::kLaneKey,
                            static_cast<uint32_t>(key - keys.begin())};
    } else {
      filler_columns_[i] = {FillerSource::kNull, 0};
    }
  }
}

OperatorState* IntervalFillGaps::NewState(Context& context) const {
  auto* state = context.Make<State>();
  state->background_input = PipelineUse(*state, spec_.background.pipeline);
  state->background.Init(spec_.background,
                         IntervalInputName{kName, "the background"}, *state);
  state->input.Init(spec_.input, IntervalInputName{kName, "the input"}, *state);
  if (broadcast_) {
    state->keys.Init(spec_.input.key_columns.span(), *state);
    state->gathered_keys = context.NewBatch(*state);
  }
  state->gathered = context.NewBatch(*state);
  state->bounds.Init(*state);
  state->rows = context.NewSpan<uint32_t>(kMaxBatchRows);
  return state;
}

bool IntervalFillGaps::EnsureBackgroundRead(State& s) const {
  if (!s.background_read) {
    base::Status status = ReadBackground(s);
    if (!status.ok()) {
      s.context().Fail(std::move(status));
    }
    s.background_read = true;
  }
  return !s.context().failed();
}

base::Status IntervalFillGaps::ReadBackground(State& s) const {
  RETURN_IF_ERROR(s.background.Collect(s.background_input, s,
                                       shared_ ? &s.lanes : nullptr,
                                       std::numeric_limits<uint32_t>::max()));
  for (uint32_t i = 0; i < s.background.size(); ++i) {
    // Intervals are sorted by start, so only the previous one with width can
    // overlap. Zero-width intervals cover nothing.
    std::optional<Ts> end;
    for (const Interval& interval : s.background.lane(i).intervals) {
      if (interval.start == interval.end) {
        continue;
      }
      if (end && interval.start < *end) {
        return base::ErrStatus("%s: background rows overlap within a lane",
                               kName);
      }
      end = interval.end;
    }
  }
  s.visited.assign(s, s.background.size(), false);
  return base::OkStatus();
}

void IntervalFillGaps::StartLane(uint32_t lane, uint32_t row, State& s) const {
  s.lane = lane;
  uint32_t tag = lane;
  if (broadcast_) {
    s.lane_key = s.keys.Of(row);
    tag = s.lane_key;
  }
  const uint32_t background_lane = broadcast_ ? 0 : lane;
  base::Span<const Interval> background;
  if (std::optional<uint32_t> i = s.background.Find(background_lane)) {
    s.visited[*i] = true;
    background = s.background.lane(*i).intervals;
  }
  s.gaps.StartLane(tag, background, KeepGap{&s.found, &s});
}

base::Status IntervalFillGaps::Cover(const RowBatch& in, State& s) const {
  RETURN_IF_ERROR(s.input.Start(in, shared_ ? &s.lanes : nullptr, s));
  LaneBatch lanes = s.input.batch();
  if (broadcast_) {
    RETURN_IF_ERROR(s.keys.Hold(in, s));
    // The open lane's keys are now carried.
    s.lane_key = LaneKeys::kCarried;
    s.gaps.set_lane(s.lane_key);
  }
  uint32_t n = in.size();
  for (uint32_t row = 0; row < n; ++row) {
    uint32_t lane = lanes.lane(row);
    if (!s.gaps.in_lane() || lane != s.lane) {
      StartLane(lane, row, s);
    }
    int64_t start;
    int64_t end;
    LaneBatch::Result result = lanes.Read(row, &start, &end);
    if (result != LaneBatch::Result::kOk) {
      if (result == LaneBatch::Result::kSkip) {
        continue;
      }
      return lanes.Error(result, {kName, "the input"});
    }
    s.gaps.Cover(static_cast<Ts>(start), static_cast<Ts>(end),
                 KeepGap{&s.found, &s});
  }
  if (broadcast_ && s.gaps.in_lane()) {
    s.keys.Carry(s.lane_key, s);
  }
  return base::OkStatus();
}

OpResult IntervalFillGaps::Execute(const RowBatch& in,
                                   RowBatch& out,
                                   OperatorState& state) const {
  auto& s = state.Cast<State>();
  out.Reset();
  if (s.serving) {
    return ServeFillers(out, s);
  }
  // Background lanes must be known before reading input.
  if (!EnsureBackgroundRead(s)) {
    return OpResult::kError;
  }
  base::Status status = Cover(in, s);
  if (!status.ok()) {
    s.context().Fail(std::move(status));
    return OpResult::kError;
  }
  if (s.shapes.empty()) {
    for (const IntervalFillGapsSpec::Output& output : spec_.outputs) {
      s.shapes.push_back(s, ColumnShape::Of(in.column(output.input)));
    }
  }
  out.Project(in, input_columns_.span());
  s.serving = !s.found.empty();
  return s.serving ? OpResult::kHaveMoreOutput : OpResult::kNeedMoreInput;
}

OpResult IntervalFillGaps::Finish(RowBatch& out, OperatorState& state) const {
  auto& s = state.Cast<State>();
  out.Reset();
  if (!EnsureBackgroundRead(s)) {
    return OpResult::kError;
  }
  if (!s.finished) {
    KeepGap keep{&s.found, &s};
    s.gaps.EndLane(keep);
    // Background lanes with no input are filled completely.
    for (uint32_t i = 0; i < s.visited.size() && !broadcast_; ++i) {
      if (!s.visited[i]) {
        s.gaps.StartLane(0, s.background.lane(i).intervals, keep);
        s.gaps.EndLane(keep);
      }
    }
    s.finished = true;
  }
  if (s.found.empty()) {
    return OpResult::kNeedMoreInput;
  }
  return ServeFillers(out, s);
}

OpResult IntervalFillGaps::ServeFillers(RowBatch& out, State& s) const {
  RunVector<IntervalGaps::Gap>& gaps = s.found;
  uint32_t count = std::min(gaps.size() - s.served, kMaxBatchRows);
  const IntervalGaps::Gap* fillers = gaps.data() + s.served;

  Context& context = s.context();
  BoundsColumns& bounds = s.bounds;
  bounds.Take(context);
  // Strided reads from the gaps, which vector instructions do not speed up.
  PERFETTO_NO_VECTORIZE_LOOP
  for (uint32_t i = 0; i < count; ++i) {
    bounds.ts[i] = static_cast<int64_t>(fillers[i].start);
    bounds.dur[i] = static_cast<int64_t>(fillers[i].end - fillers[i].start);
    s.rows[i] = fillers[i].id;
  }
  const uint32_t* rows = s.rows.begin();
  s.background.rows().Gather(s.gathered, {rows, rows + count}, s);
  if (broadcast_) {
    PERFETTO_NO_VECTORIZE_LOOP
    for (uint32_t i = 0; i < count; ++i) {
      s.rows[i] = fillers[i].lane;
    }
    s.keys.Gather(s.gathered_keys, {rows, rows + count}, s);
  }

  for (uint32_t i = 0; i < spec_.outputs.size(); ++i) {
    const FillerColumn& filler = filler_columns_[i];
    switch (filler.source) {
      case FillerSource::kTs:
        bounds.AddTsTo(&out);
        break;
      case FillerSource::kDur:
        bounds.AddDurTo(&out);
        break;
      case FillerSource::kBackground: {
        const ColumnView& column = s.gathered->column(filler.index);
        // Lowering only converts typed columns, so untyped ones can still
        // differ between the input and the background.
        if (!s.shapes.empty() &&
            !SameLogicalType(ColumnShape::Of(column), s.shapes[i])) {
          context.Fail(base::ErrStatus(
              "%s: column '%.*s' holds a different type in the background than "
              "in the input",
              kName, static_cast<int>(spec_.outputs[i].name.size()),
              spec_.outputs[i].name.data()));
          return OpResult::kError;
        }
        out.AddColumn(column, s.gathered->buffer(filler.index));
        break;
      }
      case FillerSource::kLaneKey:
        out.AddColumn(s.gathered_keys->column(filler.index),
                      s.gathered_keys->buffer(filler.index));
        break;
      case FillerSource::kNull:
        out.AddNullColumnLike(s.shapes.empty() ? nullptr : &s.shapes[i],
                              spec_.outputs[i].fallback, context);
        break;
    }
  }
  bounds.Clear();
  out.SetRowCount(count);
  s.served += count;
  if (s.served < gaps.size()) {
    return OpResult::kHaveMoreOutput;
  }
  gaps.clear();
  s.served = 0;
  s.serving = false;
  return OpResult::kNeedMoreInput;
}

}  // namespace perfetto::trace_processor::core::exec
