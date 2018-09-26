/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/sched_slice_table.h"

#include <string.h>
#include <algorithm>
#include <bitset>
#include <numeric>

#include "perfetto/base/logging.h"
#include "perfetto/base/utils.h"
#include "src/trace_processor/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

namespace {

using namespace sqlite_utils;

constexpr uint64_t kUint64Max = std::numeric_limits<uint64_t>::max();

template <size_t N = base::kMaxCpus>
bool PopulateFilterBitmap(int op,
                          sqlite3_value* value,
                          std::bitset<N>* filter) {
  bool constraint_implemented = true;
  int64_t int_value = sqlite3_value_int64(value);
  if (IsOpGe(op) || IsOpGt(op)) {
    // If the operator is gt, then add one to the upper bound.
    int_value = IsOpGt(op) ? int_value + 1 : int_value;

    // Set to false all values less than |int_value|.
    size_t ub = static_cast<size_t>(std::max<int64_t>(0, int_value));
    ub = std::min(ub, filter->size());
    for (size_t i = 0; i < ub; i++) {
      filter->set(i, false);
    }
  } else if (IsOpLe(op) || IsOpLt(op)) {
    // If the operator is lt, then minus one to the lower bound.
    int_value = IsOpLt(op) ? int_value - 1 : int_value;

    // Set to false all values greater than |int_value|.
    size_t lb = static_cast<size_t>(std::max<int64_t>(0, int_value));
    lb = std::min(lb, filter->size());
    for (size_t i = lb; i < filter->size(); i++) {
      filter->set(i, false);
    }
  } else if (IsOpEq(op)) {
    if (int_value >= 0 && static_cast<size_t>(int_value) < filter->size()) {
      // If the value is in bounds, set all bits to false and restore the value
      // of the bit at the specified index.
      bool existing = filter->test(static_cast<size_t>(int_value));
      filter->reset();
      filter->set(static_cast<size_t>(int_value), existing);
    } else {
      // If the index is out of bounds, nothing should match.
      filter->reset();
    }
  } else {
    constraint_implemented = false;
  }
  return constraint_implemented;
}

template <class T>
inline int Compare(T first, T second, bool desc) {
  if (first < second) {
    return desc ? 1 : -1;
  } else if (first > second) {
    return desc ? -1 : 1;
  }
  return 0;
}

}  // namespace

SchedSliceTable::SchedSliceTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void SchedSliceTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<SchedSliceTable>(db, storage, "sched");
}

Table::Schema SchedSliceTable::CreateSchema(int, const char* const*) {
  return Schema(
      {
          Table::Column(Column::kTimestamp, "ts", ColumnType::kUlong),
          Table::Column(Column::kCpu, "cpu", ColumnType::kUint),
          Table::Column(Column::kDuration, "dur", ColumnType::kUlong),
          Table::Column(Column::kUtid, "utid", ColumnType::kUint),
      },
      {Column::kCpu, Column::kTimestamp});
}

std::unique_ptr<Table::Cursor> SchedSliceTable::CreateCursor() {
  return std::unique_ptr<Table::Cursor>(new Cursor(storage_));
}

int SchedSliceTable::BestIndex(const QueryConstraints& qc,
                               BestIndexInfo* info) {
  bool is_time_constrained = false;
  for (size_t i = 0; i < qc.constraints().size(); i++) {
    const auto& cs = qc.constraints()[i];
    if (cs.iColumn == Column::kTimestamp)
      is_time_constrained = true;
  }

  info->estimated_cost = is_time_constrained ? 10 : 10000;
  info->order_by_consumed = true;

  return SQLITE_OK;
}

SchedSliceTable::Cursor::Cursor(const TraceStorage* storage)
    : storage_(storage) {}

int SchedSliceTable::Cursor::Filter(const QueryConstraints& qc,
                                    sqlite3_value** argv) {
  filter_state_.reset(new FilterState(storage_, qc, argv));
  return SQLITE_OK;
}

int SchedSliceTable::Cursor::Next() {
  filter_state_->FindNextSlice();
  return SQLITE_OK;
}

int SchedSliceTable::Cursor::Eof() {
  return !filter_state_->IsNextRowIdIndexValid();
}

int SchedSliceTable::Cursor::Column(sqlite3_context* context, int N) {
  PERFETTO_DCHECK(filter_state_->IsNextRowIdIndexValid());

  size_t row = filter_state_->next_row_id();
  const auto& slices = storage_->slices();
  switch (N) {
    case Column::kTimestamp: {
      uint64_t ts = slices.start_ns()[row];
      sqlite3_result_int64(context, static_cast<sqlite3_int64>(ts));
      break;
    }
    case Column::kCpu: {
      sqlite3_result_int(context, static_cast<int>(slices.cpus()[row]));
      break;
    }
    case Column::kDuration: {
      uint64_t duration = slices.durations()[row];
      sqlite3_result_int64(context, static_cast<sqlite3_int64>(duration));
      break;
    }
    case Column::kUtid: {
      sqlite3_result_int64(context, slices.utids()[row]);
      break;
    }
  }
  return SQLITE_OK;
}

SchedSliceTable::FilterState::FilterState(
    const TraceStorage* storage,
    const QueryConstraints& query_constraints,
    sqlite3_value** argv)
    : order_by_(query_constraints.order_by()), storage_(storage) {
  // Remove ordering on timestamp if it is the only ordering as we are already
  // sorted on TS. This makes span joining significantly faster.
  if (order_by_.size() == 1 && order_by_[0].iColumn == Column::kTimestamp &&
      !order_by_[0].desc) {
    order_by_.clear();
  }

  std::bitset<base::kMaxCpus> cpu_filter;
  cpu_filter.set();

  uint64_t min_ts = 0;
  uint64_t max_ts = kUint64Max;

  for (size_t i = 0; i < query_constraints.constraints().size(); i++) {
    const auto& cs = query_constraints.constraints()[i];
    switch (cs.iColumn) {
      case Column::kCpu:
        PopulateFilterBitmap(cs.op, argv[i], &cpu_filter);
        break;
      case Column::kTimestamp: {
        auto ts = static_cast<uint64_t>(sqlite3_value_int64(argv[i]));
        if (IsOpGe(cs.op) || IsOpGt(cs.op)) {
          min_ts = IsOpGe(cs.op) ? ts : ts + 1;
        } else if (IsOpLe(cs.op) || IsOpLt(cs.op)) {
          max_ts = IsOpLe(cs.op) ? ts : ts - 1;
        }
        break;
      }
    }
  }
  SetupSortedRowIds(min_ts, max_ts);

  // Filter rows on CPUs if any CPUs need to be excluded.
  const auto& slices = storage_->slices();
  row_filter_.resize(sorted_row_ids_.size(), true);
  if (cpu_filter.count() < cpu_filter.size()) {
    for (size_t i = 0; i < sorted_row_ids_.size(); i++) {
      row_filter_[i] = cpu_filter.test(slices.cpus()[sorted_row_ids_[i]]);
    }
  }
  FindNextRowAndTimestamp();
}

void SchedSliceTable::FilterState::SetupSortedRowIds(uint64_t min_ts,
                                                     uint64_t max_ts) {
  const auto& slices = storage_->slices();
  const auto& start_ns = slices.start_ns();
  PERFETTO_CHECK(slices.slice_count() <= std::numeric_limits<uint32_t>::max());

  auto min_it = std::lower_bound(start_ns.begin(), start_ns.end(), min_ts);
  auto max_it = std::upper_bound(min_it, start_ns.end(), max_ts);
  ptrdiff_t dist = std::distance(min_it, max_it);
  PERFETTO_CHECK(dist >= 0 && static_cast<size_t>(dist) <= start_ns.size());

  // Fill |indices| with the consecutive row numbers affected by the filtering.
  sorted_row_ids_.resize(static_cast<size_t>(dist));
  std::iota(sorted_row_ids_.begin(), sorted_row_ids_.end(),
            std::distance(start_ns.begin(), min_it));

  // Sort if there is any order by constraints.
  if (!order_by_.empty()) {
    std::sort(
        sorted_row_ids_.begin(), sorted_row_ids_.end(),
        [this](uint32_t f, uint32_t s) { return CompareSlices(f, s) < 0; });
  }
}

int SchedSliceTable::FilterState::CompareSlices(size_t f_idx, size_t s_idx) {
  for (const auto& ob : order_by_) {
    int c = CompareSlicesOnColumn(f_idx, s_idx, ob);
    if (c != 0)
      return c;
  }
  return 0;
}

int SchedSliceTable::FilterState::CompareSlicesOnColumn(
    size_t f_idx,
    size_t s_idx,
    const QueryConstraints::OrderBy& ob) {
  const auto& sl = storage_->slices();
  switch (ob.iColumn) {
    case SchedSliceTable::Column::kTimestamp:
      return Compare(sl.start_ns()[f_idx], sl.start_ns()[s_idx], ob.desc);
    case SchedSliceTable::Column::kDuration:
      return Compare(sl.durations()[f_idx], sl.durations()[s_idx], ob.desc);
    case SchedSliceTable::Column::kCpu:
      return Compare(sl.cpus()[f_idx], sl.cpus()[s_idx], ob.desc);
    case SchedSliceTable::Column::kUtid:
      return Compare(sl.utids()[f_idx], sl.utids()[s_idx], ob.desc);
  }
  PERFETTO_FATAL("Unexpected column %d", ob.iColumn);
}

void SchedSliceTable::FilterState::FindNextSlice() {
  next_row_id_index_++;
  FindNextRowAndTimestamp();
}

void SchedSliceTable::FilterState::FindNextRowAndTimestamp() {
  auto start =
      row_filter_.begin() +
      static_cast<decltype(row_filter_)::difference_type>(next_row_id_index_);
  auto next_it = std::find(start, row_filter_.end(), true);
  next_row_id_index_ =
      static_cast<uint32_t>(std::distance(row_filter_.begin(), next_it));
}

}  // namespace trace_processor
}  // namespace perfetto
