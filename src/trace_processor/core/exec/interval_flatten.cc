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
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/small_vector.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/util/heap.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

bool IsInt64(const RowBatch& batch, uint32_t index) {
  const ColumnView& column = batch.column(index);
  return column.kind() == ColumnView::Kind::kFlat && column.type().Is<Int64>();
}

constexpr auto kEndsLater = [](const auto& a, const auto& b) {
  return a.end > b.end;
};

template <typename Live>
void PushEnd(FlexVector<Live>& heap, Live live) {
  heap.push_back(live);
  HeapSiftUp(heap.data(), heap.size() - 1, live, kEndsLater);
}

template <typename Live>
void PopEnd(FlexVector<Live>& heap) {
  Live last = heap.back();
  heap.pop_back();
  if (!heap.empty()) {
    HeapSiftDown(heap.data(), heap.size(), last, kEndsLater);
  }
}

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
  state->instant = state->live;
  state->segment_sums.resize(sums_);
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
  FlatColumnReader<int64_t> ts(in.column(spec_.ts_column));
  FlatColumnReader<int64_t> dur(in.column(spec_.dur_column));
  base::SmallVector<FlatColumnReader<int64_t>, 4> sums;
  for (const IntervalFlattenSpec::Aggregate& agg : spec_.aggregates) {
    if (agg.function == IntervalFlattenSpec::Function::kSum) {
      sums.emplace_back(in.column(agg.column));
    }
  }
  bool keyed = !spec_.key_columns.empty();
  const ColumnView* groups = keyed ? &in.column(spec_.group_column) : nullptr;
  bool started = false;
  uint32_t rows = in.size();
  for (uint32_t row = 0; row < rows; ++row) {
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
      if (s.instant.count > 0) {
        EmitInstant(s);
      }
      Advance(s, start);
      s.time = start;
    }
    Totals* totals = &s.instant;
    uint32_t slot = 0;
    if (length > 0) {
      totals = &s.live;
      if (s.free_slots.empty()) {
        slot = s.slots++;
        s.slot_sums.resize(uint64_t{s.slots} * sums_);
      } else {
        slot = s.free_slots.back();
        s.free_slots.pop_back();
      }
      PushEnd(s.ends, Live{start + length, slot});
    }
    ++totals->count;
    for (uint32_t i = 0; i < sums_; ++i) {
      Sum held{};
      held.holding = sums[i].Read(row, &held.sum);
      totals->sums[i].sum += held.sum;
      totals->sums[i].holding += held.holding;
      if (length > 0) {
        s.slot_sums[size_t{slot} * sums_ + i] = held;
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
  int64_t previous = s.previous;
  while (!s.ends.empty() && s.ends.begin()->end <= time) {
    int64_t end = s.ends.begin()->end;
    if (s.live.count > 0 && previous < end) {
      Emit(s, previous, end - previous, false);
    }
    previous = end;
    while (!s.ends.empty() && s.ends.begin()->end == end) {
      uint32_t slot = s.ends.begin()->slot;
      PopEnd(s.ends);
      --s.live.count;
      for (uint32_t i = 0; i < sums_; ++i) {
        const Sum& held = s.slot_sums[size_t{slot} * sums_ + i];
        s.live.sums[i].sum -= held.sum;
        s.live.sums[i].holding -= held.holding;
      }
      s.free_slots.push_back(slot);
    }
  }
  if (s.live.count > 0 && previous < time) {
    Emit(s, previous, time - previous, false);
  }
  s.previous = time;
}

void IntervalFlatten::EmitInstant(State& s) const {
  Emit(s, s.time, 0, true);
  s.instant.count = 0;
  std::fill(s.instant.sums.begin(), s.instant.sums.end(), Sum());
}

void IntervalFlatten::EndGroup(State& s) const {
  if (s.instant.count > 0) {
    EmitInstant(s);
  }
  Advance(s, std::numeric_limits<int64_t>::max());
}

void IntervalFlatten::Emit(State& s,
                           int64_t ts,
                           int64_t dur,
                           bool with_instant) const {
  uint32_t n = s.segments;
  if (PERFETTO_UNLIKELY(n == s.segment_capacity)) {
    GrowSegments(s);
  }
  s.segment_ts[n] = ts;
  s.segment_dur[n] = dur;
  s.segment_groups[n] = s.group_number;
  s.segment_key_rows[n] = s.key_row;
  const Totals& live = s.live;
  const Totals& instant = s.instant;
  s.segment_counts[n] = live.count + (with_instant ? instant.count : 0);
  for (uint32_t i = 0; i < sums_; ++i) {
    Sum total = live.sums[i];
    if (with_instant) {
      total.sum += instant.sums[i].sum;
      total.holding += instant.sums[i].holding;
    }
    State::SegmentSums& out = s.segment_sums[i];
    out.values[n] = total.sum;
    out.present.change(n, total.holding > 0);
  }
  s.segments = n + 1;
}

PERFETTO_NO_INLINE void IntervalFlatten::GrowSegments(State& s) const {
  s.segment_capacity = std::max(64u, s.segment_capacity * 2);
  s.segment_ts.resize(s.segment_capacity);
  s.segment_dur.resize(s.segment_capacity);
  s.segment_groups.resize(s.segment_capacity);
  s.segment_key_rows.resize(s.segment_capacity);
  s.segment_counts.resize(s.segment_capacity);
  for (State::SegmentSums& sum : s.segment_sums) {
    sum.values.resize(s.segment_capacity);
    sum.present.resize(s.segment_capacity);
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
  uint32_t count = std::min(s.segments - s.served, kMaxBatchRows);
  if (count == 0) {
    return false;
  }
  auto add = [&](StorageType type, const void* values,
                 const BitVector* validity) {
    ColumnView view = ColumnView::Reference(type, values, validity);
    view.SetRange(s.served);
    out.AddColumn(view);
  };
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
    if (spec_.aggregates[a].function == IntervalFlattenSpec::Function::kSum) {
      const State::SegmentSums& sum = s.segment_sums[sum_index_[a]];
      add(StorageType{Int64{}}, sum.values.data(), &sum.present);
    } else {
      add(StorageType{Int64{}}, s.segment_counts.data(), nullptr);
    }
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
  std::fill(s.live.sums.begin(), s.live.sums.end(), Sum());
  s.instant.count = 0;
  std::fill(s.instant.sums.begin(), s.instant.sums.end(), Sum());
  s.ends.clear();
  s.slots = 0;
  s.slot_sums.clear();
  s.free_slots.clear();
  s.key_rows.Clear();
  s.segments = 0;
  s.key_row = 0;
  s.served = 0;
}

}  // namespace perfetto::trace_processor::core::exec
