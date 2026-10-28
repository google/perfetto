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

#include "src/trace_processor/core/exec/interval_lanes.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/run_held.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

namespace {

// "<op>: <input>", with the input's number if it has one.
base::StackString<128> Prefix(const IntervalInputName& name) {
  auto op = static_cast<int>(name.op.size());
  auto input = static_cast<int>(name.input.size());
  if (name.number) {
    return base::StackString<128>("%.*s: %.*s %u", op, name.op.data(), input,
                                  name.input.data(), name.number);
  }
  return base::StackString<128>("%.*s: %.*s", op, name.op.data(), input,
                                name.input.data());
}

}  // namespace

base::Status LaneBatch::Error(Result result,
                              const IntervalInputName& name) const {
  if (result == Result::kOverflow) {
    return base::ErrStatus("%.*s: integer overflow",
                           static_cast<int>(name.op.size()), name.op.data());
  }
  PERFETTO_DCHECK(result == Result::kNegative);
  if (forever_) {
    return base::ErrStatus(
        "%s has a row whose ts is below zero, or whose dur is below zero and "
        "not -1",
        Prefix(name).c_str());
  }
  return base::ErrStatus("%s has a row whose ts or dur is below zero",
                         Prefix(name).c_str());
}

base::Status LaneReader::Start(const RowBatch& in, GroupTable* shared) {
  PERFETTO_DCHECK(IsFlat<Int64>(in.column(spec_->ts_column)));
  PERFETTO_DCHECK(IsFlat<Int64>(in.column(spec_->dur_column)));
  batch_ = &in;
  shared_ = shared && !spec_->key_columns.empty();
  if (!shared_) {
    return base::OkStatus();
  }
  // A lane's rows arrive together, so only the first row of each lane is
  // looked up in the shared table.
  first_rows_.clear();
  uint32_t last = last_group_;
  for (uint32_t row = 0; row < in.size(); ++row) {
    uint32_t group = in.Value<uint32_t>(spec_->group_column, row);
    if (group != last) {
      first_rows_.push_back(row);
      last = group;
    }
  }
  if (!first_rows_.empty()) {
    firsts_.CopyFrom(in.selection());
    const uint32_t* firsts = first_rows_.data();
    firsts_.Keep({firsts, firsts + first_rows_.size()});
    first_lanes_.resize(first_rows_.size());
    if (std::optional<uint32_t> bad = shared->FindOrCreate(
            in, firsts_, spec_->key_columns, first_lanes_.data())) {
      return base::ErrStatus(
          "%s's PER column %u must hold one type, the same in every input",
          Prefix(name_).c_str(), *bad + 1);
    }
  }
  lanes_.resize(in.size());
  size_t next = 0;
  for (uint32_t row = 0; row < in.size(); ++row) {
    if (next < first_rows_.size() && first_rows_[next] == row) {
      last_lane_ = first_lanes_[next++];
    }
    lanes_[row] = last_lane_;
  }
  last_group_ = last;
  return base::OkStatus();
}

base::Status CollectedLanes::Collect(OperatorState& state,
                                     Context& context,
                                     GroupTable* shared,
                                     uint32_t limit) {
  const Source& source = *input_->source;
  while (source.GetData(batch_, state)) {
    RETURN_IF_ERROR(reader_.Start(batch_, shared));
    LaneBatch lanes = reader_.batch();
    uint32_t first_row = rows_.size();
    for (uint32_t row = 0; row < batch_.size(); ++row) {
      int64_t start;
      int64_t end;
      LaneBatch::Result result = lanes.Read(row, &start, &end);
      if (result != LaneBatch::Result::kOk) {
        if (result == LaneBatch::Result::kSkip) {
          continue;
        }
        return lanes.Error(result, reader_.name());
      }
      uint32_t number = lanes.lane(row);
      if (number >= limit) {
        continue;
      }
      Add(number, Interval{static_cast<Ts>(start), static_cast<Ts>(end),
                           first_row + row});
    }
    retained_.Project(batch_, input_->retained_columns);
    RETURN_IF_ERROR(rows_.Append(retained_, context));
  }
  RETURN_IF_ERROR(source.status(state));
  by_number_.clear();
  by_number_.reserve(ranges_.size());
  for (uint32_t i = 0; i < ranges_.size(); ++i) {
    by_number_.emplace_back(ranges_[i].number, i);
  }
  if (!std::is_sorted(by_number_.begin(), by_number_.end())) {
    std::sort(by_number_.begin(), by_number_.end());
  }
  // Find() returns one range per number, so each lane must arrive whole.
  PERFETTO_DCHECK(std::adjacent_find(by_number_.begin(), by_number_.end(),
                                     [](const auto& a, const auto& b) {
                                       return a.first == b.first;
                                     }) == by_number_.end());
  return base::OkStatus();
}

PERFETTO_ALWAYS_INLINE void CollectedLanes::Add(uint32_t number,
                                                const Interval& interval) {
  if (ranges_.empty() || ranges_.back().number != number) {
    auto at = static_cast<uint32_t>(intervals_.size());
    ranges_.push_back({number, at, at, true});
  }
  Range& range = ranges_.back();
  // Intervals arrive sorted by start, so comparing with the previous one is
  // enough to detect overlap.
  if (range.end > range.begin && IsOverlapping(intervals_.back(), interval)) {
    range.nonoverlapping = false;
  }
  intervals_.push_back(interval);
  range.end++;
}

void CollectedLanes::EndRun() {
  reader_.Reset();
  EndRunOf(&intervals_);
  EndRunOf(&ranges_);
  EndRunOf(&by_number_);
  rows_.EndRun();
}

void CollectedLanes::Clear() {
  reader_.Reset();
  intervals_.clear();
  ranges_.clear();
  by_number_.clear();
  rows_.Clear();
}

base::Status LaneKeys::Hold(const RowBatch& in, Context& context) {
  rows_.Clear();
  // The open lane's keys may be in buffers the previous batch has since
  // given back for reuse, so they were kept by Carry().
  if (carried_.size()) {
    RETURN_IF_ERROR(rows_.Append(carried_, context));
  }
  first_ = rows_.size();
  retained_.Project(in, *key_columns_);
  return rows_.Append(retained_, context);
}

void LaneKeys::Carry(uint32_t handle) {
  rows_.View(&carried_, handle, 1);
}

void LaneKeys::Gather(RowBatch* out, Span<const uint32_t> handles, Context& c) {
  rows_.Gather(out, handles, c);
}

void LaneKeys::EndRun() {
  Clear();
  rows_.EndRun();
}

void LaneKeys::Clear() {
  rows_.Clear();
  retained_.Reset();
  carried_.Reset();
  first_ = 0;
}

void BoundsColumns::Take(Context& context) {
  ts_buffer_ = context.TakeBuffer(sizeof(int64_t));
  dur_buffer_ = context.TakeBuffer(sizeof(int64_t));
  ts = ts_buffer_.chunk().Values<int64_t>();
  dur = dur_buffer_.chunk().Values<int64_t>();
}

void BoundsColumns::AddTo(RowBatch* out) {
  out->AddColumn(ColumnView::Reference(StorageType{Int64{}}, ts),
                 std::move(ts_buffer_));
  out->AddColumn(ColumnView::Reference(StorageType{Int64{}}, dur),
                 std::move(dur_buffer_));
  ts = nullptr;
  dur = nullptr;
}

void BoundsColumns::AddTsTo(RowBatch* out) const {
  out->AddColumn(ColumnView::Reference(StorageType{Int64{}}, ts), ts_buffer_);
}

void BoundsColumns::AddDurTo(RowBatch* out) const {
  out->AddColumn(ColumnView::Reference(StorageType{Int64{}}, dur), dur_buffer_);
}

void BoundsColumns::EndRun() {
  Clear();
}

void BoundsColumns::Clear() {
  ts_buffer_ = ColumnBuffer();
  dur_buffer_ = ColumnBuffer();
  ts = nullptr;
  dur = nullptr;
}

}  // namespace perfetto::trace_processor::core::exec
