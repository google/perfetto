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
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/key_encoder.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr int64_t kEndOfTime = std::numeric_limits<int64_t>::max();

// A span of time, [start, end).
struct Span64 {
  int64_t start;
  int64_t end;
};

// A background row's span and where its columns are retained.
struct BackgroundSpan {
  int64_t start;
  int64_t end;
  uint32_t row;
};

// A span of the background no input covers, and where its values come from.
struct Filler {
  int64_t start;
  int64_t end;
  uint32_t background_row;
  // In the lane keys, when the background has no PER columns of its own.
  uint32_t key_row;
};

// The kind and type of a column, which every batch of it must agree on.
struct Shape {
  ColumnView::Kind kind;
  StorageType type;
};

Shape ShapeOf(const ColumnView& column) {
  return {column.kind(), column.type()};
}

// As SameLogicalType: an implicit Id and a stored Uint32 hold the same values.
bool SameShape(const Shape& a, const Shape& b) {
  auto is_uint32 = [](const Shape& s) {
    return s.kind != ColumnView::Kind::kVariant &&
           (s.type.Is<Id>() || s.type.Is<Uint32>());
  };
  if (is_uint32(a) && is_uint32(b)) {
    return true;
  }
  return a.kind == b.kind &&
         (a.kind == ColumnView::Kind::kVariant || a.type == b.type);
}

bool IsInt64(const RowBatch& batch, uint32_t index) {
  const ColumnView& column = batch.column(index);
  return column.kind() == ColumnView::Kind::kFlat && column.type().Is<Int64>();
}

// The input's coverage and the background of one lane.
struct Lane {
  std::string key;
  std::vector<Span64> covered;
  std::vector<BackgroundSpan> background;
  uint32_t key_row = 0;
};

// Removes `covered`, sorted and merged, from each of `background`, sorted and
// never overlapping, adding a filler for every piece left.
void Subtract(const std::vector<BackgroundSpan>& background,
              const std::vector<Span64>& covered,
              uint32_t key_row,
              std::vector<Filler>* out) {
  size_t next = 0;
  for (const BackgroundSpan& span : background) {
    // Coverage ending before this span cannot reach any later one either.
    while (next < covered.size() && covered[next].end <= span.start) {
      ++next;
    }
    int64_t cursor = span.start;
    for (size_t i = next; i < covered.size() && covered[i].start < span.end;
         ++i) {
      if (covered[i].start > cursor) {
        out->push_back({cursor, covered[i].start, span.row, key_row});
      }
      cursor = std::max(cursor, covered[i].end);
      if (cursor >= span.end) {
        break;
      }
    }
    if (cursor < span.end) {
      out->push_back({cursor, span.end, span.row, key_row});
    }
  }
}

// Sorts spans by start and merges those which overlap or abut.
void Merge(std::vector<Span64>* spans) {
  std::sort(spans->begin(), spans->end(),
            [](const Span64& a, const Span64& b) { return a.start < b.start; });
  size_t merged = 0;
  for (const Span64& span : *spans) {
    if (merged > 0 && span.start <= (*spans)[merged - 1].end) {
      (*spans)[merged - 1].end = std::max((*spans)[merged - 1].end, span.end);
    } else {
      (*spans)[merged++] = span;
    }
  }
  spans->resize(merged);
}

}  // namespace

struct IntervalFillGaps::State : OperatorState {
  State() : OperatorState(ResetEachRun{}) {}
  ~State() override;
  void Reset() override;

  const Source* background = nullptr;
  std::unique_ptr<OperatorState> background_state;
  base::Status status = base::OkStatus();
  bool read = false;
  bool filled = false;

  // Shared by the input and the background, so their keys' types must agree.
  KeyEncoder keys;
  // Held by pointer, so the keys the map views never move.
  std::vector<std::unique_ptr<Lane>> lanes;
  base::FlatHashMapV2<std::string_view, Lane*> by_key;
  // The background, when it has no PER columns and so applies to every lane.
  std::vector<BackgroundSpan> shared;
  RowStore background_rows;
  // One row of the input's PER columns per lane, read when the background
  // has none of its own.
  RowStore lane_keys;

  // The shapes outputs take on each side, by output, once a batch shows them.
  std::vector<std::optional<Shape>> input_shapes;
  std::vector<std::optional<Shape>> background_shapes;

  std::vector<Filler> fillers;
  size_t served = 0;

  // Buffers reused across calls; nothing in them outlives the next call.
  RowBatch batch;
  RowBatch retained;
  std::vector<uint32_t> new_lane_rows;
  std::vector<uint32_t> rows;
  FlexVector<int64_t> ts;
  FlexVector<int64_t> dur;
  RowBatch gathered_background;
  RowBatch gathered_keys;

  Lane& GetLane(std::string_view key, bool* added) {
    if (Lane** found = by_key.Find(key)) {
      *added = false;
      return **found;
    }
    lanes.emplace_back(std::make_unique<Lane>());
    Lane& lane = *lanes.back();
    lane.key.assign(key);
    by_key.Insert(lane.key, &lane);
    *added = true;
    return lane;
  }
};

IntervalFillGaps::State::~State() = default;

void IntervalFillGaps::State::Reset() {
  background->Rewind(*background_state);
  status = base::OkStatus();
  read = false;
  filled = false;
  keys = KeyEncoder();
  by_key.Clear();
  lanes.clear();
  shared.clear();
  background_rows.Clear();
  lane_keys.Clear();
  std::fill(input_shapes.begin(), input_shapes.end(), std::nullopt);
  std::fill(background_shapes.begin(), background_shapes.end(), std::nullopt);
  fillers.clear();
  served = 0;
}

IntervalFillGaps::IntervalFillGaps(IntervalFillGapsSpec spec)
    : Operator({BatchPreference::kThroughput}), spec_(std::move(spec)) {
  PERFETTO_CHECK(spec_.background_key_columns.empty() ||
                 spec_.background_key_columns.size() ==
                     spec_.key_columns.size());
  for (const IntervalFillGapsSpec::Output& output : spec_.outputs) {
    std::optional<uint32_t> retained;
    if (output.background) {
      auto it = std::find(retained_columns_.begin(), retained_columns_.end(),
                          *output.background);
      retained = static_cast<uint32_t>(it - retained_columns_.begin());
      if (it == retained_columns_.end()) {
        retained_columns_.push_back(*output.background);
      }
    }
    retained_of_output_.push_back(retained);
    std::optional<uint32_t> key;
    if (output.input) {
      auto it = std::find(spec_.key_columns.begin(), spec_.key_columns.end(),
                          *output.input);
      if (it != spec_.key_columns.end()) {
        key = static_cast<uint32_t>(it - spec_.key_columns.begin());
      }
    }
    key_of_output_.push_back(key);
  }
  // Wide enough for any storage type's values.
  zeros_.resize(kMaxBatchRows);
  std::fill(zeros_.begin(), zeros_.end(), 0);
  null_variants_.resize(kMaxBatchRows);
  std::fill(null_variants_.begin(), null_variants_.end(), Variant::Null());
  no_values_ = BitVector::CreateWithSize(kMaxBatchRows, false);
}

IntervalFillGaps::~IntervalFillGaps() = default;

std::unique_ptr<OperatorState> IntervalFillGaps::MakeState() const {
  auto state = std::make_unique<State>();
  state->background = spec_.background;
  state->background_state = spec_.background->MakeState();
  state->input_shapes.resize(spec_.outputs.size());
  state->background_shapes.resize(spec_.outputs.size());
  return state;
}

base::Status IntervalFillGaps::status(const OperatorState& state) const {
  return state.Cast<const State>().status;
}

ColumnView IntervalFillGaps::Nulls(bool variant, StorageType type) const {
  if (variant) {
    return ColumnView::Variants(null_variants_.data());
  }
  // An Id column has no storage to be null in; a Uint32 holds the same values.
  if (type.Is<Id>()) {
    type = StorageType{Uint32{}};
  }
  return ColumnView::Reference(type, zeros_.data(), &no_values_);
}

// Fails if an output's input and background columns hold different types,
// once both sides have shown theirs.
base::Status IntervalFillGaps::CheckShapes(const State& s) const {
  for (size_t i = 0; i < spec_.outputs.size(); ++i) {
    const auto& in = s.input_shapes[i];
    const auto& bg = s.background_shapes[i];
    if (in && bg && !SameShape(*in, *bg)) {
      return base::ErrStatus(
          "INTERVAL FILL GAPS: column '%s' holds a different type in the "
          "background than in the input",
          spec_.outputs[i].name.c_str());
    }
  }
  return base::OkStatus();
}

base::Status IntervalFillGaps::ReadBackground(State& s) const {
  RowBatch& batch = s.batch;
  bool keyed = !spec_.background_key_columns.empty();
  while (spec_.background->GetData(batch, *s.background_state)) {
    if (!IsInt64(batch, spec_.background_ts_column) ||
        !IsInt64(batch, spec_.background_dur_column)) {
      return base::ErrStatus(
          "INTERVAL FILL GAPS: the background's ts and dur must be Int64");
    }
    for (size_t i = 0; i < spec_.outputs.size(); ++i) {
      if (spec_.outputs[i].background && !s.background_shapes[i]) {
        s.background_shapes[i] =
            ShapeOf(batch.column(*spec_.outputs[i].background));
      }
    }
    if (keyed) {
      if (std::optional<uint32_t> bad =
              s.keys.Encode(batch, spec_.background_key_columns)) {
        return base::ErrStatus(
            "INTERVAL FILL GAPS: PER column %u must hold one type, the same "
            "in the background as in the input",
            *bad + 1);
      }
    }
    FlatColumnReader<int64_t> ts(batch.column(spec_.background_ts_column));
    FlatColumnReader<int64_t> dur(batch.column(spec_.background_dur_column));
    uint32_t first_row = s.background_rows.size();
    for (uint32_t row = 0; row < batch.size(); ++row) {
      int64_t start;
      int64_t length;
      if (!ts.Read(row, &start) || !dur.Read(row, &length)) {
        continue;
      }
      if (start < 0 || length < 0) {
        return base::ErrStatus(
            "INTERVAL FILL GAPS: a background row's ts or dur is below zero");
      }
      int64_t end;
      if (!base::CheckedAdd(start, length, &end)) {
        return base::ErrStatus("INTERVAL FILL GAPS: integer overflow");
      }
      if (length == 0) {
        continue;
      }
      BackgroundSpan span{start, end, first_row + row};
      if (keyed) {
        bool added;
        s.GetLane(s.keys.Key(row), &added).background.push_back(span);
      } else {
        s.shared.push_back(span);
      }
    }
    s.retained.Reset();
    for (uint32_t column : retained_columns_) {
      s.retained.AddColumn(batch.column(column), batch.owner(column));
    }
    s.retained.SetCardinality(batch.size());
    RETURN_IF_ERROR(s.background_rows.Append(s.retained));
  }
  RETURN_IF_ERROR(spec_.background->status(*s.background_state));

  auto sort = [](std::vector<BackgroundSpan>* spans) -> base::Status {
    std::sort(spans->begin(), spans->end(),
              [](const BackgroundSpan& a, const BackgroundSpan& b) {
                return a.start < b.start;
              });
    for (size_t i = 1; i < spans->size(); ++i) {
      if ((*spans)[i].start < (*spans)[i - 1].end) {
        return base::ErrStatus(
            "INTERVAL FILL GAPS: background rows overlap within a lane");
      }
    }
    return base::OkStatus();
  };
  RETURN_IF_ERROR(sort(&s.shared));
  for (const auto& lane : s.lanes) {
    RETURN_IF_ERROR(sort(&lane->background));
  }
  return CheckShapes(s);
}

base::Status IntervalFillGaps::Cover(const RowBatch& in, State& s) const {
  if (!IsInt64(in, spec_.ts_column) || !IsInt64(in, spec_.dur_column)) {
    return base::ErrStatus("INTERVAL FILL GAPS: ts and dur must be Int64");
  }
  bool shapes_known = true;
  for (size_t i = 0; i < spec_.outputs.size(); ++i) {
    if (spec_.outputs[i].input && !s.input_shapes[i]) {
      s.input_shapes[i] = ShapeOf(in.column(*spec_.outputs[i].input));
      shapes_known = false;
    }
  }
  if (!shapes_known) {
    RETURN_IF_ERROR(CheckShapes(s));
  }
  bool keyed = !spec_.key_columns.empty();
  if (keyed) {
    if (std::optional<uint32_t> bad = s.keys.Encode(in, spec_.key_columns)) {
      return base::ErrStatus(
          "INTERVAL FILL GAPS: PER column %u must hold one type, the same in "
          "the input as in the background",
          *bad + 1);
    }
  }
  // Each lane's PER values are kept when the background cannot supply them.
  bool keep_keys = keyed && spec_.background_key_columns.empty();
  s.new_lane_rows.clear();
  FlatColumnReader<int64_t> ts(in.column(spec_.ts_column));
  FlatColumnReader<int64_t> dur(in.column(spec_.dur_column));
  for (uint32_t row = 0; row < in.size(); ++row) {
    bool added;
    Lane& lane =
        s.GetLane(keyed ? s.keys.Key(row) : std::string_view(), &added);
    if (added && keep_keys) {
      lane.key_row =
          s.lane_keys.size() + static_cast<uint32_t>(s.new_lane_rows.size());
      s.new_lane_rows.push_back(row);
    }
    int64_t start;
    int64_t length;
    if (!ts.Read(row, &start) || !dur.Read(row, &length)) {
      continue;
    }
    if (start < 0 || length < -1) {
      return base::ErrStatus(
          "INTERVAL FILL GAPS: a row's ts is below zero, or its dur is below "
          "zero and not -1");
    }
    int64_t end = kEndOfTime;
    if (length >= 0 && !base::CheckedAdd(start, length, &end)) {
      return base::ErrStatus("INTERVAL FILL GAPS: integer overflow");
    }
    if (length != 0) {
      lane.covered.push_back({start, end});
    }
  }
  if (!s.new_lane_rows.empty()) {
    s.retained.Reset();
    for (uint32_t column : spec_.key_columns) {
      s.retained.AddColumn(in.column(column), in.owner(column));
    }
    s.retained.SetCardinality(in.size());
    const uint32_t* rows = s.new_lane_rows.data();
    s.retained.Slice(
        RowSelection::Indices({rows, rows + s.new_lane_rows.size()}),
        static_cast<uint32_t>(s.new_lane_rows.size()));
    RETURN_IF_ERROR(s.lane_keys.Append(s.retained));
  }
  return base::OkStatus();
}

OpResult IntervalFillGaps::Execute(const RowBatch& in,
                                   RowBatch& out,
                                   OperatorState& state) const {
  auto& s = state.Cast<State>();
  out.Reset();
  if (s.status.ok() && !s.read) {
    s.status = ReadBackground(s);
    s.read = true;
  }
  if (s.status.ok()) {
    s.status = Cover(in, s);
  }
  if (!s.status.ok()) {
    return OpResult::kError;
  }
  // Input rows pass through: a background-only column is null on them.
  for (size_t i = 0; i < spec_.outputs.size(); ++i) {
    const IntervalFillGapsSpec::Output& output = spec_.outputs[i];
    if (output.input) {
      out.AddColumn(in.column(*output.input), in.owner(*output.input));
    } else {
      const std::optional<Shape>& like = s.background_shapes[i];
      out.AddColumn(Nulls(like && like->kind == ColumnView::Kind::kVariant,
                          like ? like->type : output.fallback));
    }
  }
  out.SetCardinality(in.size());
  return OpResult::kNeedMoreInput;
}

void IntervalFillGaps::Fill(State& s) const {
  for (const auto& lane : s.lanes) {
    Merge(&lane->covered);
  }
  if (!spec_.background_key_columns.empty()) {
    // The background names the lanes; input in no lane of it fills nothing.
    for (const auto& lane : s.lanes) {
      Subtract(lane->background, lane->covered, 0, &s.fillers);
    }
    return;
  }
  if (spec_.key_columns.empty() && s.lanes.empty()) {
    // Without PER there is one lane, filled whole when nothing covers it.
    Subtract(s.shared, {}, 0, &s.fillers);
    return;
  }
  for (const auto& lane : s.lanes) {
    Subtract(s.shared, lane->covered, lane->key_row, &s.fillers);
  }
}

OpResult IntervalFillGaps::Finish(RowBatch& out, OperatorState& state) const {
  auto& s = state.Cast<State>();
  out.Reset();
  if (s.status.ok() && !s.read) {
    s.status = ReadBackground(s);
    s.read = true;
  }
  if (!s.status.ok()) {
    return OpResult::kError;
  }
  if (!s.filled) {
    Fill(s);
    s.filled = true;
  }
  auto count = static_cast<uint32_t>(
      std::min<size_t>(s.fillers.size() - s.served, kMaxBatchRows));
  if (count == 0) {
    return OpResult::kNeedMoreInput;
  }
  const Filler* fillers = s.fillers.data() + s.served;
  s.ts.resize(count);
  s.dur.resize(count);
  s.rows.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    s.ts[i] = fillers[i].start;
    s.dur[i] = fillers[i].end - fillers[i].start;
    s.rows[i] = fillers[i].background_row;
  }
  if (!retained_columns_.empty()) {
    s.background_rows.View(&s.gathered_background,
                           {s.rows.data(), s.rows.data() + count});
  }
  bool lane_keys = s.lane_keys.size() > 0;
  if (lane_keys) {
    for (uint32_t i = 0; i < count; ++i) {
      s.rows[i] = fillers[i].key_row;
    }
    s.lane_keys.View(&s.gathered_keys, {s.rows.data(), s.rows.data() + count});
  }
  for (size_t i = 0; i < spec_.outputs.size(); ++i) {
    const IntervalFillGapsSpec::Output& output = spec_.outputs[i];
    if (std::optional<uint32_t> r = retained_of_output_[i]) {
      out.AddColumn(s.gathered_background.column(*r),
                    s.gathered_background.owner(*r));
    } else if (output.input == spec_.ts_column) {
      out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, s.ts.data()));
    } else if (output.input == spec_.dur_column) {
      out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, s.dur.data()));
    } else if (key_of_output_[i] && lane_keys) {
      out.AddColumn(s.gathered_keys.column(*key_of_output_[i]),
                    s.gathered_keys.owner(*key_of_output_[i]));
    } else {
      const std::optional<Shape>& like = s.input_shapes[i];
      out.AddColumn(Nulls(like && like->kind == ColumnView::Kind::kVariant,
                          like ? like->type : output.fallback));
    }
  }
  out.SetCardinality(count);
  s.served += count;
  return s.served < s.fillers.size() ? OpResult::kHaveMoreOutput
                                     : OpResult::kNeedMoreInput;
}

}  // namespace perfetto::trace_processor::core::exec
