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

#include "src/trace_processor/core/exec/interval_flatten.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/interval_columns.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

bool IsInt64(const RowBatch& batch, uint32_t index) {
  const ColumnView& column = batch.column(index);
  return column.kind() == ColumnView::Kind::kFlat && column.type().Is<Int64>();
}

struct EndsLater {
  template <typename Live>
  bool operator()(const Live& a, const Live& b) const {
    return a.end > b.end;
  }
};

}  // namespace

IntervalFlatten::IntervalFlatten(IntervalFlattenSpec spec)
    : spec_(std::move(spec)) {
  for (const IntervalFlattenSpec::Aggregate& agg : spec_.aggregates) {
    sum_index_.push_back(sums_);
    if (agg.function == IntervalFlattenSpec::Function::kSum) {
      ++sums_;
    }
  }
}

IntervalFlatten::~IntervalFlatten() = default;
IntervalFlatten::State::~State() = default;

std::unique_ptr<Breaker::State> IntervalFlatten::CreateState() const {
  auto state = std::make_unique<State>();
  state->live.sums.resize(sums_);
  state->live.holding.resize(sums_);
  state->instant = state->live;
  state->segment_values.resize(spec_.aggregates.size());
  return state;
}

bool IntervalFlatten::Consume(const RowBatch& in, Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  bool typed = IsInt64(in, spec_.ts_column) && IsInt64(in, spec_.dur_column);
  for (const IntervalFlattenSpec::Aggregate& agg : spec_.aggregates) {
    typed = typed && (agg.function != IntervalFlattenSpec::Function::kSum ||
                      IsInt64(in, agg.column));
  }
  if (!typed) {
    s.status = base::ErrStatus(
        "INTERVAL FLATTEN: ts, dur and summed columns must be Int64");
    return false;
  }
  Int64Reader ts(in.column(spec_.ts_column));
  Int64Reader dur(in.column(spec_.dur_column));
  std::vector<Int64Reader> sums;
  for (const IntervalFlattenSpec::Aggregate& agg : spec_.aggregates) {
    if (agg.function == IntervalFlattenSpec::Function::kSum) {
      sums.emplace_back(in.column(agg.column));
    }
  }
  bool keyed = !spec_.key_columns.empty();
  const ColumnView* groups = keyed ? &in.column(spec_.group_column) : nullptr;
  bool started = false;
  for (uint32_t row = 0; row < in.size(); ++row) {
    int64_t start;
    int64_t length;
    if (!ts.Read(row, &start) || !dur.Read(row, &length)) {
      continue;
    }
    if (start < 0 || length < 0) {
      s.status =
          base::ErrStatus("INTERVAL FLATTEN: a row's ts or dur is below zero");
      return false;
    }
    uint32_t group = keyed ? groups->Value<uint32_t>(row) : 0;
    if (!s.in_group || group != s.group) {
      if (s.in_group) {
        EndGroup(s);
        ++s.group_number;
      }
      s.in_group = true;
      s.group = group;
      s.key_row = s.key_rows.size() + row;
      s.time = std::numeric_limits<int64_t>::min();
      started = true;
    }
    if (start != s.time) {
      if (start < s.time) {
        s.status = base::ErrStatus(
            "INTERVAL FLATTEN: a group's rows must arrive in order of ts");
        return false;
      }
      EmitInstant(s);
      Advance(s, start);
      s.time = start;
    }
    Totals* totals = &s.instant;
    uint32_t slot = 0;
    if (length > 0) {
      totals = &s.live;
      if (s.free_slots.empty()) {
        slot = s.slots++;
        s.slot_values.resize(uint64_t{s.slots} * sums_);
        s.slot_present.resize(uint64_t{s.slots} * sums_);
      } else {
        slot = s.free_slots.back();
        s.free_slots.pop_back();
      }
      s.ends.push_back({start + length, slot});
      std::push_heap(s.ends.begin(), s.ends.end(), EndsLater());
    }
    ++totals->count;
    for (uint32_t i = 0; i < sums_; ++i) {
      int64_t value = 0;
      bool present = sums[i].Read(row, &value);
      if (present) {
        totals->sums[i] += value;
        ++totals->holding[i];
      }
      if (length > 0) {
        size_t at = size_t{slot} * sums_ + i;
        s.slot_values[at] = value;
        s.slot_present[at] = present;
      }
    }
  }
  // Keys are read back from the row each group starts at.
  if (!keyed || !started) {
    return true;
  }
  s.retained.Reset();
  for (uint32_t column : spec_.key_columns) {
    s.retained.AddColumn(in.column(column), in.owner(column));
  }
  s.retained.SetCardinality(in.size());
  s.status = s.key_rows.Append(s.retained);
  return s.status.ok();
}

void IntervalFlatten::Advance(State& s, int64_t time) const {
  while (!s.ends.empty() && s.ends.begin()->end <= time) {
    int64_t end = s.ends.begin()->end;
    if (s.live.count > 0 && s.previous < end) {
      Emit(s, s.previous, end - s.previous, false);
    }
    s.previous = end;
    while (!s.ends.empty() && s.ends.begin()->end == end) {
      uint32_t slot = s.ends.begin()->slot;
      std::pop_heap(s.ends.begin(), s.ends.end(), EndsLater());
      s.ends.pop_back();
      --s.live.count;
      for (uint32_t i = 0; i < sums_; ++i) {
        size_t at = size_t{slot} * sums_ + i;
        if (s.slot_present[at]) {
          s.live.sums[i] -= s.slot_values[at];
          --s.live.holding[i];
        }
      }
      s.free_slots.push_back(slot);
    }
  }
  if (s.live.count > 0 && s.previous < time) {
    Emit(s, s.previous, time - s.previous, false);
  }
  s.previous = time;
}

void IntervalFlatten::EmitInstant(State& s) const {
  if (s.instant.count == 0) {
    return;
  }
  Emit(s, s.time, 0, true);
  s.instant.count = 0;
  std::fill(s.instant.sums.begin(), s.instant.sums.end(), 0);
  std::fill(s.instant.holding.begin(), s.instant.holding.end(), 0);
}

void IntervalFlatten::EndGroup(State& s) const {
  EmitInstant(s);
  Advance(s, std::numeric_limits<int64_t>::max());
}

void IntervalFlatten::Emit(State& s,
                           int64_t ts,
                           int64_t dur,
                           bool with_instant) const {
  s.segment_ts.push_back(ts);
  s.segment_dur.push_back(dur);
  s.segment_groups.push_back(s.group_number);
  if (!spec_.key_columns.empty()) {
    s.segment_key_rows.push_back(s.key_row);
  }
  const Totals& live = s.live;
  const Totals& instant = s.instant;
  for (uint32_t a = 0; a < spec_.aggregates.size(); ++a) {
    State::Values& out = s.segment_values[a];
    switch (spec_.aggregates[a].function) {
      case IntervalFlattenSpec::Function::kCount:
        out.values.push_back(live.count + (with_instant ? instant.count : 0));
        break;
      case IntervalFlattenSpec::Function::kSum: {
        uint32_t i = sum_index_[a];
        int64_t holding =
            live.holding[i] + (with_instant ? instant.holding[i] : 0);
        out.values.push_back(live.sums[i] +
                             (with_instant ? instant.sums[i] : 0));
        out.present.push_back(holding > 0);
        break;
      }
    }
  }
}

bool IntervalFlatten::Finalize(Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  if (s.in_group) {
    EndGroup(s);
  }
  return true;
}

bool IntervalFlatten::Serve(RowBatch& out, Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  auto segments = static_cast<uint32_t>(s.segment_ts.size());
  uint32_t count = std::min(segments - s.served, kMaxBatchRows);
  if (count == 0) {
    return false;
  }
  auto add = [&](StorageType type, const void* values,
                 const BitVector* validity) {
    ColumnView view = ColumnView::Reference(type, values, validity);
    view.SetRange(s.served);
    out.AddColumn(view);
  };
  out.Reset();
  add(StorageType{Int64{}}, s.segment_ts.data(), nullptr);
  add(StorageType{Int64{}}, s.segment_dur.data(), nullptr);
  if (!spec_.key_columns.empty()) {
    const uint32_t* rows = s.segment_key_rows.data() + s.served;
    s.key_rows.View(&s.served_keys, {rows, rows + count});
    for (uint32_t k = 0; k < s.served_keys.column_count(); ++k) {
      out.AddColumn(s.served_keys.column(k));
    }
  }
  for (uint32_t a = 0; a < spec_.aggregates.size(); ++a) {
    const State::Values& values = s.segment_values[a];
    bool sum =
        spec_.aggregates[a].function == IntervalFlattenSpec::Function::kSum;
    add(StorageType{Int64{}}, values.values.data(),
        sum ? &values.present : nullptr);
  }
  add(StorageType{Uint32{}}, s.segment_groups.data(), nullptr);
  out.SetCardinality(count);
  s.served += count;
  return true;
}

void IntervalFlatten::Reset(Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  s.group = 0;
  s.group_number = 0;
  s.in_group = false;
  s.previous = 0;
  s.time = 0;
  s.live.count = 0;
  std::fill(s.live.sums.begin(), s.live.sums.end(), 0);
  std::fill(s.live.holding.begin(), s.live.holding.end(), 0);
  s.instant = s.live;
  s.ends.clear();
  s.slots = 0;
  s.slot_values.clear();
  s.slot_present.clear();
  s.free_slots.clear();
  s.key_rows.Clear();
  s.segment_ts.clear();
  s.segment_dur.clear();
  s.segment_groups.clear();
  s.segment_key_rows.clear();
  s.key_row = 0;
  for (State::Values& values : s.segment_values) {
    values.values.clear();
    values.present = BitVector();
  }
  s.served = 0;
}

}  // namespace perfetto::trace_processor::core::exec
