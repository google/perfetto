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
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/interval_lanes.h"
#include "src/trace_processor/core/exec/run_held.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/interval_gaps.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr char kName[] = "INTERVAL FILL GAPS";

// Adds a null column of the same kind and type as `like`, or of `fallback` if
// `like` is null.
void AddNulls(const ColumnShape* like,
              StorageType fallback,
              Context& context,
              RowBatch* out) {
  if (like && like->kind == ColumnView::Kind::kVariant) {
    // Zeroed, the null buffer's values are null Variants.
    const ColumnBuffer& nulls = context.NullBuffer();
    out->AddColumn(ColumnView::Variants(nulls.chunk().Values<Variant>()),
                   nulls);
    return;
  }
  out->AddNullColumn(like ? like->type : fallback, context);
}

}  // namespace

struct IntervalFillGaps::State : OperatorState {
  State(const IntervalFillGapsSpec& spec, Context& c)
      : context(&c),
        background(this,
                   spec.background,
                   IntervalInputName{kName, "the background"}),
        input(this, spec.input, IntervalInputName{kName, "the input"}),
        keys(this, spec.input.key_columns) {}
  ~State() override;

  Context* context;
  std::unique_ptr<OperatorState> background_state;

  Held<base::Status> status{this};
  // Numbers lanes by key on both sides, if both have PER columns.
  Held<GroupTable> lanes{this};

  // The background, read in full before any input.
  RunValue<bool> background_read{this, false};
  Held<CollectedLanes> background;
  // Whether each background lane had any input.
  Held<std::vector<bool>> visited{this};

  // The input, a lane at a time.
  RunValue<bool> finished{this, false};
  Held<LaneReader> input;
  // If broadcasting, the PER values of the input batch's lanes, and the
  // handle of the current lane's.
  Held<LaneKeys> keys;
  RunValue<uint32_t> lane_key{this};
  // The current input lane.
  RunValue<uint32_t> lane{this};
  Held<IntervalGaps> gaps{this};

  // The fillers served.
  // The shape of each output's input column, from the first input batch:
  // empty until then.
  Held<std::vector<ColumnShape>> shapes{this};
  // How many gaps have been output, and whether the current input batch has
  // been output but its gaps have not.
  RunValue<size_t> served{this};
  RunValue<bool> serving{this, false};
  Held<RowBatch> gathered{this};
  Held<RowBatch> gathered_keys{this};

  // Scratch for one call: the rows of a filler being gathered.
  CallScratch<std::vector<uint32_t>> rows;
};

IntervalFillGaps::State::~State() = default;

IntervalFillGaps::IntervalFillGaps(IntervalFillGapsSpec spec)
    : spec_(std::move(spec)) {
  const auto& keys = spec_.input.key_columns;
  const auto& background_keys = spec_.background.lanes.key_columns;
  PERFETTO_CHECK(background_keys.empty() ||
                 background_keys.size() == keys.size());
  broadcast_ = !keys.empty() && background_keys.empty();
  shared_ = !keys.empty() && !broadcast_;
  for (const IntervalFillGapsSpec::Output& output : spec_.outputs) {
    input_columns_.push_back(output.input);
    // ts and dur always come from the filler, not the background.
    auto key = std::find(keys.begin(), keys.end(), output.input);
    if (output.input == spec_.input.ts_column) {
      filler_columns_.push_back({FillerSource::kTs, 0});
    } else if (output.input == spec_.input.dur_column) {
      filler_columns_.push_back({FillerSource::kDur, 0});
    } else if (output.background != IntervalFillGapsSpec::Output::kNone) {
      filler_columns_.push_back({FillerSource::kBackground, output.background});
    } else if (broadcast_ && key != keys.end()) {
      filler_columns_.push_back(
          {FillerSource::kLaneKey, static_cast<uint32_t>(key - keys.begin())});
    } else {
      filler_columns_.push_back({FillerSource::kNull, 0});
    }
  }
}

IntervalFillGaps::~IntervalFillGaps() = default;

std::unique_ptr<OperatorState> IntervalFillGaps::MakeState(
    Context& context) const {
  auto state = std::make_unique<State>(spec_, context);
  state->background_state = spec_.background.source->MakeState(context);
  return state;
}

base::Status IntervalFillGaps::status(const OperatorState& state) const {
  return state.Cast<const State>().status;
}

bool IntervalFillGaps::EnsureBackgroundRead(State& s) const {
  if (!s.background_read) {
    Restart(*spec_.background.source, *s.background_state);
    s.status = ReadBackground(s);
    s.background_read = true;
  }
  return s.status.ok();
}

base::Status IntervalFillGaps::ReadBackground(State& s) const {
  RETURN_IF_ERROR(s.background.Collect(*s.background_state, *s.context,
                                       shared_ ? &s.lanes : nullptr));
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
  s.visited.assign(s.background.size(), false);
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
  Span<const Interval> background;
  if (std::optional<uint32_t> i = s.background.Find(background_lane)) {
    s.visited[*i] = true;
    background = s.background.lane(*i).intervals;
  }
  s.gaps.StartLane(tag, background);
}

base::Status IntervalFillGaps::Cover(const RowBatch& in, State& s) const {
  RETURN_IF_ERROR(s.input.Start(in, shared_ ? &s.lanes : nullptr));
  LaneBatch lanes = s.input.batch();
  if (broadcast_) {
    RETURN_IF_ERROR(s.keys.Hold(in, *s.context));
    // The open lane's keys are now carried.
    s.lane_key = LaneKeys::kCarried;
    s.gaps.set_lane(s.lane_key);
  }
  for (uint32_t row = 0; row < in.size(); ++row) {
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
    s.gaps.Cover(static_cast<Ts>(start), static_cast<Ts>(end));
  }
  if (broadcast_ && s.gaps.in_lane()) {
    s.keys.Carry(s.lane_key);
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
  s.status = Cover(in, s);
  if (!s.status.ok()) {
    return OpResult::kError;
  }
  if (s.shapes.empty()) {
    for (const IntervalFillGapsSpec::Output& output : spec_.outputs) {
      s.shapes.push_back(ColumnShape::Of(in.column(output.input)));
    }
  }
  out.Project(in, input_columns_);
  s.serving = !s.gaps.gaps().empty();
  return s.serving ? OpResult::kHaveMoreOutput : OpResult::kNeedMoreInput;
}

OpResult IntervalFillGaps::Finish(RowBatch& out, OperatorState& state) const {
  auto& s = state.Cast<State>();
  out.Reset();
  if (!EnsureBackgroundRead(s)) {
    return OpResult::kError;
  }
  if (!s.finished) {
    s.gaps.EndLane();
    // Background lanes with no input are filled completely.
    for (uint32_t i = 0; i < s.visited.size() && !broadcast_; ++i) {
      if (!s.visited[i]) {
        s.gaps.StartLane(0, s.background.lane(i).intervals);
        s.gaps.EndLane();
      }
    }
    s.finished = true;
  }
  if (s.gaps.gaps().empty()) {
    return OpResult::kNeedMoreInput;
  }
  return ServeFillers(out, s);
}

OpResult IntervalFillGaps::ServeFillers(RowBatch& out, State& s) const {
  std::vector<IntervalGaps::Gap>& gaps = s.gaps.gaps();
  auto count = static_cast<uint32_t>(
      std::min<size_t>(gaps.size() - s.served, kMaxBatchRows));
  const IntervalGaps::Gap* fillers = gaps.data() + s.served;

  BoundsColumns bounds;
  bounds.Take(*s.context);
  s.rows.resize(count);
  // Strided reads from the gaps, which vector instructions do not speed up.
  PERFETTO_NO_VECTORIZE_LOOP
  for (uint32_t i = 0; i < count; ++i) {
    bounds.ts[i] = static_cast<int64_t>(fillers[i].start);
    bounds.dur[i] = static_cast<int64_t>(fillers[i].end - fillers[i].start);
    s.rows[i] = fillers[i].id;
  }
  const uint32_t* rows = s.rows.data();
  s.background.rows().Gather(&s.gathered, {rows, rows + count}, *s.context);
  if (broadcast_) {
    PERFETTO_NO_VECTORIZE_LOOP
    for (uint32_t i = 0; i < count; ++i) {
      s.rows[i] = fillers[i].lane;
    }
    s.keys.Gather(&s.gathered_keys, {rows, rows + count}, *s.context);
  }

  for (size_t i = 0; i < spec_.outputs.size(); ++i) {
    const FillerColumn& filler = filler_columns_[i];
    switch (filler.source) {
      case FillerSource::kTs:
        bounds.AddTsTo(&out);
        break;
      case FillerSource::kDur:
        bounds.AddDurTo(&out);
        break;
      case FillerSource::kBackground: {
        const ColumnView& column = s.gathered.column(filler.index);
        // Lowering only converts typed columns, so untyped ones can still
        // differ between the input and the background.
        if (!s.shapes.empty() &&
            !SameLogicalType(ColumnShape::Of(column), s.shapes[i])) {
          s.status = base::ErrStatus(
              "%s: column '%s' holds a different type in the background than "
              "in the input",
              kName, spec_.outputs[i].name.c_str());
          return OpResult::kError;
        }
        out.AddColumn(column, s.gathered.buffer(filler.index));
        break;
      }
      case FillerSource::kLaneKey:
        out.AddColumn(s.gathered_keys.column(filler.index),
                      s.gathered_keys.buffer(filler.index));
        break;
      case FillerSource::kNull:
        AddNulls(s.shapes.empty() ? nullptr : &s.shapes[i],
                 spec_.outputs[i].fallback, *s.context, &out);
        break;
    }
  }
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
