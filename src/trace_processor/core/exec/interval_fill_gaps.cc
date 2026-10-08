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

// How it works
// ============
//
// Rows are split into lanes by their PER values, and each lane is filled on
// its own, in three steps:
//
//  1. Before the first input row, the background is read in full. Each row's
//     span goes to its lane, and its columns are retained for the fillers.
//     Once read, each lane's background is sorted and checked not to overlap.
//  2. Each input batch passes through unchanged, and each row's span is
//     recorded as covered in its lane.
//  3. In Finish(), each lane's coverage is subtracted from its background
//     with one walk over both, and each piece left becomes a filler. Fillers
//     take their bounds from the piece, their other columns from the
//     background row the piece came from and, when the background has no PER
//     columns, their PER values from the lane.
//
// For example, with a background row [0, 100) and input rows [10, 20),
// [15, 30) and [50, 60) in one lane, the lane's coverage is [10, 30) and
// [50, 60), and the fillers are [0, 10), [30, 50) and [60, 100).
//
// Input usually arrives in order of ts, so coverage is merged as it is
// recorded: a span starting inside or at the end of the last one extends it.
// A lane's coverage is then already sorted and merged by Finish(), with one
// entry per run of touching rows rather than one per row; only a lane whose
// input arrived out of order is sorted there.

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr int64_t kEndOfTime = std::numeric_limits<int64_t>::max();

// A span of time, [start, end).
struct Span64 {
  int64_t start;
  int64_t end;
};

// A background row's span, and the row of its columns in the retained rows.
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
  // The lane's row in `lane_keys`, when every lane shares the background.
  uint32_t key_row;
};

bool IsInt64(const RowBatch& batch, uint32_t index) {
  const ColumnView& column = batch.column(index);
  return column.kind() == ColumnView::Kind::kFlat && column.type().Is<Int64>();
}

// The rows of one lane: what its input covers, and what it is filled over.
struct Lane {
  // Records [start, end) as covered.
  void Cover(int64_t start, int64_t end) {
    if (!covered.empty()) {
      Span64& last = covered.back();
      if (start >= last.start && start <= last.end) {
        last.end = std::max(last.end, end);
        return;
      }
      covered_in_order &= start > last.end;
    }
    covered.push_back({start, end});
  }

  // The lane's PER values, encoded. Empty without PER.
  std::string key;
  // Sorted, and no two overlapping or touching, while `covered_in_order`.
  std::vector<Span64> covered;
  bool covered_in_order = true;
  // Unused when every lane shares one background.
  std::vector<BackgroundSpan> background;
  // When every lane shares one background, the lane's row in `lane_keys`.
  uint32_t key_row = 0;
};

// Sorts spans by start and merges those which overlap or touch.
void SortAndMerge(std::vector<Span64>* spans) {
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

// Sorts spans by start, failing if two overlap.
base::Status SortBackground(std::vector<BackgroundSpan>* spans) {
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
}

// Adds a filler for each piece of `background` which `covered` leaves
// uncovered. Both are sorted, and neither has two spans which overlap, so one
// walk over both does: `next` only moves forward, as coverage ending before
// one background span ends before every later one too.
void AddGaps(const std::vector<BackgroundSpan>& background,
             const std::vector<Span64>& covered,
             uint32_t key_row,
             std::vector<Filler>* out) {
  size_t next = 0;
  for (const BackgroundSpan& span : background) {
    while (next < covered.size() && covered[next].end <= span.start) {
      ++next;
    }
    // Walk the coverage overlapping this span. `gap_start` is where the next
    // filler would begin: the end of the coverage seen so far.
    int64_t gap_start = span.start;
    for (size_t i = next; i < covered.size() && covered[i].start < span.end;
         ++i) {
      if (covered[i].start > gap_start) {
        out->push_back({gap_start, covered[i].start, span.row, key_row});
      }
      gap_start = std::max(gap_start, covered[i].end);
    }
    if (gap_start < span.end) {
      out->push_back({gap_start, span.end, span.row, key_row});
    }
  }
}

}  // namespace

struct IntervalFillGaps::State : OperatorState {
  State() : OperatorState(ResetEachRun{}) {}
  ~State() override;
  void Reset() override;

  Lane* FindLane(std::string_view key) {
    Lane** found = by_key.Find(key);
    return found ? *found : nullptr;
  }

  Lane& AddLane(std::string_view key) {
    lanes.emplace_back(std::make_unique<Lane>());
    Lane& lane = *lanes.back();
    lane.key.assign(key);
    by_key.Insert(lane.key, &lane);
    return lane;
  }

  const Source* background = nullptr;
  std::unique_ptr<OperatorState> background_state;
  base::Status status = base::OkStatus();
  bool background_read = false;
  bool filled = false;

  // Encodes the PER values of both sides, so their types must agree.
  KeyEncoder keys;
  // Every lane, by its encoded PER values.
  // Held by pointer, so the keys `by_key` views never move.
  std::vector<std::unique_ptr<Lane>> lanes;
  base::FlatHashMapV2<std::string_view, Lane*> by_key;
  // When the background does not name the lanes, the one they share.
  std::vector<BackgroundSpan> shared_background;
  // The background's columns which fillers read, a row per background row.
  RowStore background_rows;
  // When the background does not name the lanes, the input's PER values, a
  // row per lane.
  RowStore lane_keys;

  // The kind and type each output shows on each side, once a batch shows it.
  std::vector<std::optional<ColumnShape>> input_shapes;
  std::vector<std::optional<ColumnShape>> background_shapes;

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
};

IntervalFillGaps::State::~State() = default;

void IntervalFillGaps::State::Reset() {
  background->Rewind(*background_state);
  status = base::OkStatus();
  background_read = false;
  filled = false;
  keys = KeyEncoder();
  by_key.Clear();
  lanes.clear();
  shared_background.clear();
  background_rows.Clear();
  lane_keys.Clear();
  std::fill(input_shapes.begin(), input_shapes.end(), std::nullopt);
  std::fill(background_shapes.begin(), background_shapes.end(), std::nullopt);
  fillers.clear();
  served = 0;
}

IntervalFillGaps::IntervalFillGaps(IntervalFillGapsSpec spec)
    : Operator({BatchPreference::kThroughput}), spec_(std::move(spec)) {
  background_names_lanes_ =
      spec_.background_key_columns.size() == spec_.key_columns.size();
  PERFETTO_CHECK(background_names_lanes_ ||
                 spec_.background_key_columns.empty());
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

ColumnView IntervalFillGaps::Nulls(const std::optional<ColumnShape>& like,
                                   StorageType fallback) const {
  if (like && like->kind == ColumnView::Kind::kVariant) {
    return ColumnView::Variants(null_variants_.data());
  }
  StorageType type = like ? like->type : fallback;
  // An Id column has no storage to be null in; a Uint32 holds the same values.
  if (type.Is<Id>()) {
    type = StorageType{Uint32{}};
  }
  return ColumnView::Reference(type, zeros_.data(), &no_values_);
}

base::Status IntervalFillGaps::RecordShapes(
    const RowBatch& batch,
    bool background,
    std::vector<std::optional<ColumnShape>>* shapes,
    const std::vector<std::optional<ColumnShape>>& other_side) const {
  for (size_t i = 0; i < spec_.outputs.size(); ++i) {
    const IntervalFillGapsSpec::Output& output = spec_.outputs[i];
    std::optional<uint32_t> column =
        background ? output.background : output.input;
    if (!column || (*shapes)[i]) {
      continue;
    }
    (*shapes)[i] = ColumnShape::Of(batch.column(*column));
    if (other_side[i] && !SameLogicalType(*(*shapes)[i], *other_side[i])) {
      return base::ErrStatus(
          "INTERVAL FILL GAPS: column '%s' holds a different type in the "
          "background than in the input",
          output.name.c_str());
    }
  }
  return base::OkStatus();
}

std::string_view IntervalFillGaps::KeyOf(const State& s, uint32_t row) const {
  return spec_.key_columns.empty() ? std::string_view() : s.keys.Key(row);
}

bool IntervalFillGaps::EnsureBackgroundRead(State& s) const {
  if (!s.background_read) {
    s.status = ReadBackground(s);
    s.background_read = true;
  }
  return s.status.ok();
}

base::Status IntervalFillGaps::ReadBackground(State& s) const {
  RowBatch& batch = s.batch;
  while (spec_.background->GetData(batch, *s.background_state)) {
    if (!IsInt64(batch, spec_.background_ts_column) ||
        !IsInt64(batch, spec_.background_dur_column)) {
      return base::ErrStatus(
          "INTERVAL FILL GAPS: the background's ts and dur must be Int64");
    }
    RETURN_IF_ERROR(RecordShapes(batch, /*background=*/true,
                                 &s.background_shapes, s.input_shapes));
    if (background_names_lanes_ && !spec_.key_columns.empty()) {
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
      if (background_names_lanes_) {
        std::string_view key = KeyOf(s, row);
        Lane* lane = s.FindLane(key);
        (lane ? *lane : s.AddLane(key)).background.push_back(span);
      } else {
        s.shared_background.push_back(span);
      }
    }
    // Every row is retained, even one with no span, so a span's row is its
    // position in the background.
    s.retained.Reset();
    for (uint32_t column : retained_columns_) {
      s.retained.AddColumn(batch.column(column), batch.owner(column));
    }
    s.retained.SetCardinality(batch.size());
    RETURN_IF_ERROR(s.background_rows.Append(s.retained));
  }
  RETURN_IF_ERROR(spec_.background->status(*s.background_state));

  RETURN_IF_ERROR(SortBackground(&s.shared_background));
  for (const auto& lane : s.lanes) {
    RETURN_IF_ERROR(SortBackground(&lane->background));
  }
  return base::OkStatus();
}

base::Status IntervalFillGaps::Cover(const RowBatch& in, State& s) const {
  if (!IsInt64(in, spec_.ts_column) || !IsInt64(in, spec_.dur_column)) {
    return base::ErrStatus("INTERVAL FILL GAPS: ts and dur must be Int64");
  }
  RETURN_IF_ERROR(RecordShapes(in, /*background=*/false, &s.input_shapes,
                               s.background_shapes));
  if (!spec_.key_columns.empty()) {
    if (std::optional<uint32_t> bad = s.keys.Encode(in, spec_.key_columns)) {
      return base::ErrStatus(
          "INTERVAL FILL GAPS: PER column %u must hold one type, the same in "
          "the input as in the background",
          *bad + 1);
    }
  }
  // The rows of this batch which start lanes, when the input names them.
  s.new_lane_rows.clear();
  FlatColumnReader<int64_t> ts(in.column(spec_.ts_column));
  FlatColumnReader<int64_t> dur(in.column(spec_.dur_column));
  for (uint32_t row = 0; row < in.size(); ++row) {
    // The row's lane: none when the background names the lanes and has none
    // for it, as nothing there is filled.
    std::string_view key = KeyOf(s, row);
    Lane* lane = s.FindLane(key);
    if (!lane && !background_names_lanes_) {
      // A lane exists once any row has its key, even a row covering
      // nothing, so it is filled whole.
      lane = &s.AddLane(key);
      lane->key_row =
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
    if (lane && length != 0) {
      lane->Cover(start, end);
    }
  }
  // The new lanes' PER values are kept for their fillers: the input's buffers
  // are only borrowed until the next batch.
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
  // The background is read first: its columns' types decide the type of the
  // nulls input rows show for columns only it has.
  if (!EnsureBackgroundRead(s)) {
    return OpResult::kError;
  }
  s.status = Cover(in, s);
  if (!s.status.ok()) {
    return OpResult::kError;
  }
  for (size_t i = 0; i < spec_.outputs.size(); ++i) {
    const IntervalFillGapsSpec::Output& output = spec_.outputs[i];
    if (output.input) {
      out.AddColumn(in.column(*output.input), in.owner(*output.input));
    } else {
      out.AddColumn(Nulls(s.background_shapes[i], output.fallback));
    }
  }
  out.SetCardinality(in.size());
  return OpResult::kNeedMoreInput;
}

void IntervalFillGaps::Fill(State& s) const {
  for (const auto& lane : s.lanes) {
    if (!lane->covered_in_order) {
      SortAndMerge(&lane->covered);
    }
    AddGaps(background_names_lanes_ ? lane->background : s.shared_background,
            lane->covered, lane->key_row, &s.fillers);
  }
}

OpResult IntervalFillGaps::Finish(RowBatch& out, OperatorState& state) const {
  auto& s = state.Cast<State>();
  out.Reset();
  if (!EnsureBackgroundRead(s)) {
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

  // Gather this batch's fillers' bounds, background rows and lane keys.
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
  bool lane_keys = !background_names_lanes_;
  if (lane_keys) {
    for (uint32_t i = 0; i < count; ++i) {
      s.rows[i] = fillers[i].key_row;
    }
    s.lane_keys.View(&s.gathered_keys, {s.rows.data(), s.rows.data() + count});
  }

  // A filler's bounds are its own, even where the background has a column
  // for them.
  for (size_t i = 0; i < spec_.outputs.size(); ++i) {
    const IntervalFillGapsSpec::Output& output = spec_.outputs[i];
    if (output.input == spec_.ts_column) {
      out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, s.ts.data()));
    } else if (output.input == spec_.dur_column) {
      out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, s.dur.data()));
    } else if (std::optional<uint32_t> r = retained_of_output_[i]) {
      out.AddColumn(s.gathered_background.column(*r),
                    s.gathered_background.owner(*r));
    } else if (std::optional<uint32_t> k = key_of_output_[i]; k && lane_keys) {
      out.AddColumn(s.gathered_keys.column(*k), s.gathered_keys.owner(*k));
    } else {
      out.AddColumn(Nulls(s.input_shapes[i], output.fallback));
    }
  }
  out.SetCardinality(count);
  s.served += count;
  return s.served < s.fillers.size() ? OpResult::kHaveMoreOutput
                                     : OpResult::kNeedMoreInput;
}

}  // namespace perfetto::trace_processor::core::exec
