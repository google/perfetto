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

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/run_vector.h"
#include "src/trace_processor/core/exec/selection.h"

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

base::Status LaneBatch::ErrorOf(Result result,
                                bool forever,
                                const IntervalInputName& name) {
  if (result == Result::kOverflow) {
    return base::ErrStatus("%.*s: integer overflow",
                           static_cast<int>(name.op.size()), name.op.data());
  }
  PERFETTO_DCHECK(result == Result::kNegative);
  if (forever) {
    return base::ErrStatus(
        "%s has a row whose ts is below zero, or whose dur is below zero and "
        "not -1",
        Prefix(name).c_str());
  }
  return base::ErrStatus("%s has a row whose ts or dur is below zero",
                         Prefix(name).c_str());
}

void LaneReader::Init(const IntervalLaneSpec& spec,
                      IntervalInputName name,
                      OperatorState& owner) {
  Context& context = owner.context();
  spec_ = &spec;
  name_ = name;
  if (!spec.key_columns.empty()) {
    lanes_ = context.NewSpan<uint32_t>(kMaxBatchRows).begin();
    firsts_ = context.New<Selection>();
    first_rows_ = context.NewSpan<uint32_t>(kMaxBatchRows).begin();
    first_lanes_ = context.NewSpan<uint32_t>(kMaxBatchRows).begin();
  }
}

base::Status LaneReader::Start(const RowBatch& in,
                               GroupTable* shared,
                               OperatorState& owner) {
  PERFETTO_DCHECK(IsFlat<Int64>(in.column(spec_->ts_column)));
  PERFETTO_DCHECK(IsFlat<Int64>(in.column(spec_->dur_column)));
  batch_ = &in;
  shared_ = shared && !spec_->key_columns.empty();
  if (!shared_) {
    return base::OkStatus();
  }
  // A lane's rows arrive together, so only the first row of each lane is
  // looked up in the shared table.
  uint32_t firsts = 0;
  uint32_t last = last_group_;
  uint32_t n = in.size();
  // Without lanes set, a LaneBatch reads the group numbers.
  LaneBatch groups(in, *spec_);
  for (uint32_t row = 0; row < n; ++row) {
    uint32_t group = groups.lane(row);
    if (group != last) {
      first_rows_[firsts++] = row;
      last = group;
    }
  }
  if (firsts) {
    firsts_->CopyFrom(in.selection());
    firsts_->Keep({first_rows_, first_rows_ + firsts});
    if (std::optional<uint32_t> bad = shared->FindOrCreate(
            in, *firsts_, spec_->key_columns, first_lanes_, owner)) {
      return base::ErrStatus(
          "%s's PER column %u must hold one type, the same in every input",
          Prefix(name_).c_str(), *bad + 1);
    }
  }
  uint32_t lane = last_lane_;
  uint32_t next = 0;
  for (uint32_t row = 0; row < n; ++row) {
    if (next < firsts && first_rows_[next] == row) {
      lane = first_lanes_[next++];
    }
    lanes_[row] = lane;
  }
  last_lane_ = lane;
  last_group_ = last;
  return base::OkStatus();
}

void CollectedLanes::Init(const CollectedInputSpec& input,
                          IntervalInputName name,
                          OperatorState& owner) {
  input_ = &input;
  reader_.Init(input.lanes, name, owner);
  retained_ = owner.context().NewBatch(owner);
}

base::Status CollectedLanes::Collect(PipelineUse& input,
                                     OperatorState& owner,
                                     GroupTable* shared,
                                     uint32_t limit) {
  Context& context = owner.context();
  while (RowBatch* batch = input.Next(context)) {
    RETURN_IF_ERROR(reader_.Start(*batch, shared, owner));
    LaneBatch lanes = reader_.batch();
    uint32_t first_row = rows_.size();
    uint32_t n = batch->size();
    for (uint32_t row = 0; row < n; ++row) {
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
      Add(number,
          Interval{static_cast<Ts>(start), static_cast<Ts>(end),
                   first_row + row},
          owner);
    }
    retained_->Project(*batch, input_->retained_columns);
    RETURN_IF_ERROR(rows_.Append(*retained_, owner));
  }
  if (context.failed()) {
    return context.error();
  }
  by_number_.resize(owner, ranges_.size());
  for (uint32_t i = 0; i < ranges_.size(); ++i) {
    by_number_[i] = {ranges_[i].number, i};
  }
  if (!std::is_sorted(by_number_.begin(), by_number_.end(), &ByNumber)) {
    std::sort(by_number_.begin(), by_number_.end(), &ByNumber);
  }
  // Find() returns one range per number, so each lane must arrive whole.
  PERFETTO_DCHECK(std::adjacent_find(by_number_.begin(), by_number_.end(),
                                     [](const Numbered& a, const Numbered& b) {
                                       return a.number == b.number;
                                     }) == by_number_.end());
  return base::OkStatus();
}

bool CollectedLanes::ByNumber(const Numbered& a, const Numbered& b) {
  return a.number < b.number;
}

std::optional<uint32_t> CollectedLanes::Find(uint32_t number) const {
  const Numbered* it = std::lower_bound(by_number_.begin(), by_number_.end(),
                                        Numbered{number, 0}, &ByNumber);
  if (it == by_number_.end() || it->number != number) {
    return std::nullopt;
  }
  return it->index;
}

PERFETTO_ALWAYS_INLINE void CollectedLanes::Add(uint32_t number,
                                                const Interval& interval,
                                                OperatorState& owner) {
  if (ranges_.empty() || ranges_.back().number != number) {
    uint32_t at = intervals_.size();
    ranges_.push_back(owner, {number, at, at, true});
  }
  Range& range = ranges_.back();
  // Intervals arrive sorted by start, so comparing with the previous one is
  // enough to detect overlap.
  if (range.end > range.begin && IsOverlapping(intervals_.back(), interval)) {
    range.nonoverlapping = false;
  }
  intervals_.push_back(owner, interval);
  range.end++;
}

void CollectedLanes::Clear() {
  reader_.Reset();
  intervals_.clear();
  ranges_.clear();
  by_number_.clear();
  rows_.Clear();
}

void LaneKeys::Init(base::Span<const uint32_t> key_columns,
                    OperatorState& owner) {
  key_columns_ = key_columns;
  retained_ = owner.context().NewBatch(owner);
  carried_ = owner.context().NewBatch(owner);
}

base::Status LaneKeys::Hold(const RowBatch& in, OperatorState& owner) {
  rows_.Clear();
  // The open lane's keys may be in buffers the previous batch has since
  // given back for reuse, so they were kept by Carry().
  if (carried_->size()) {
    RETURN_IF_ERROR(rows_.Append(*carried_, owner));
  }
  first_ = rows_.size();
  retained_->Project(in, key_columns_);
  return rows_.Append(*retained_, owner);
}

void LaneKeys::Carry(uint32_t handle, OperatorState& owner) {
  rows_.View(carried_, handle, 1, owner);
}

void LaneKeys::Gather(RowBatch* out,
                      base::Span<const uint32_t> handles,
                      OperatorState& owner) {
  rows_.Gather(out, handles, owner);
}

void BoundsColumns::Init(OperatorState& owner) {
  buffers_ = owner.context().NewBuffers(owner, 2);
}

void BoundsColumns::Take(Context& context) {
  buffers_[0] = context.TakeBuffer(sizeof(int64_t));
  buffers_[1] = context.TakeBuffer(sizeof(int64_t));
  ts = buffers_[0].chunk().Values<int64_t>();
  dur = buffers_[1].chunk().Values<int64_t>();
}

void BoundsColumns::AddTo(RowBatch* out) {
  out->AddColumn(ColumnView::Reference(StorageType{Int64{}}, ts),
                 std::move(buffers_[0]));
  out->AddColumn(ColumnView::Reference(StorageType{Int64{}}, dur),
                 std::move(buffers_[1]));
  ts = nullptr;
  dur = nullptr;
}

void BoundsColumns::AddTsTo(RowBatch* out) const {
  out->AddColumn(ColumnView::Reference(StorageType{Int64{}}, ts), buffers_[0]);
}

void BoundsColumns::AddDurTo(RowBatch* out) const {
  out->AddColumn(ColumnView::Reference(StorageType{Int64{}}, dur), buffers_[1]);
}

void BoundsColumns::Clear() {
  buffers_[0] = ColumnBuffer();
  buffers_[1] = ColumnBuffer();
  ts = nullptr;
  dur = nullptr;
}

}  // namespace perfetto::trace_processor::core::exec
