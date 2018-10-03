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

template <class T>
inline int Compare(T first, T second, bool desc) {
  if (first < second) {
    return desc ? 1 : -1;
  } else if (first > second) {
    return desc ? -1 : 1;
  }
  return 0;
}

// Compares the slice at index |f| with the slice at index |s| on the
// criteria in |order_by|.
// Returns -1 if the first slice is before the second in the ordering, 1 if
// the first slice is after the second and 0 if they are equal.
PERFETTO_ALWAYS_INLINE int CompareSlicesOnColumn(
    const TraceStorage* storage,
    size_t f_idx,
    size_t s_idx,
    const QueryConstraints::OrderBy& ob) {
  const auto& sl = storage->slices();
  switch (ob.iColumn) {
    case SchedSliceTable::Column::kTimestamp:
      return Compare(sl.start_ns()[f_idx], sl.start_ns()[s_idx], ob.desc);
    case SchedSliceTable::Column::kDuration:
      return Compare(sl.durations()[f_idx], sl.durations()[s_idx], ob.desc);
    case SchedSliceTable::Column::kCpu:
      return Compare(sl.cpus()[f_idx], sl.cpus()[s_idx], ob.desc);
    case SchedSliceTable::Column::kUtid:
      return Compare(sl.utids()[f_idx], sl.utids()[s_idx], ob.desc);
    default:
      PERFETTO_FATAL("Unexpected column %d", ob.iColumn);
  }
}

// Compares the slice at index |f| with the slice at index |s|on all
// columns.
// Returns -1 if the first slice is before the second in the ordering, 1 if
// the first slice is after the second and 0 if they are equal.
PERFETTO_ALWAYS_INLINE int CompareSlices(
    const TraceStorage* storage,
    size_t f_idx,
    size_t s_idx,
    const std::vector<QueryConstraints::OrderBy>& order_by) {
  for (const auto& ob : order_by) {
    int c = CompareSlicesOnColumn(storage, f_idx, s_idx, ob);
    if (c != 0)
      return c;
  }
  return 0;
}

std::pair<uint64_t, uint64_t> GetTsBounds(const QueryConstraints& qc,
                                          sqlite3_value** argv) {
  uint64_t min_ts = 0;
  uint64_t max_ts = kUint64Max;
  for (size_t i = 0; i < qc.constraints().size(); i++) {
    const auto& cs = qc.constraints()[i];
    switch (cs.iColumn) {
      case SchedSliceTable::Column::kTimestamp:
        auto ts = static_cast<uint64_t>(sqlite3_value_int64(argv[i]));
        if (IsOpGe(cs.op) || IsOpGt(cs.op)) {
          min_ts = IsOpGe(cs.op) ? ts : ts + 1;
        } else if (IsOpLe(cs.op) || IsOpLt(cs.op)) {
          max_ts = IsOpLe(cs.op) ? ts : ts - 1;
        } else if (IsOpEq(cs.op)) {
          min_ts = ts;
          max_ts = ts;
        } else {
          // We can't handle any other constraints on ts.
          PERFETTO_CHECK(false);
        }
        break;
    }
  }
  return std::make_pair(min_ts, max_ts);
}

std::pair<uint32_t, uint32_t> FindTsIndices(
    const TraceStorage* storage,
    std::pair<uint64_t, uint64_t> ts_bounds) {
  const auto& slices = storage->slices();
  const auto& ts = slices.start_ns();
  PERFETTO_CHECK(slices.slice_count() <= std::numeric_limits<uint32_t>::max());

  auto min_it = std::lower_bound(ts.begin(), ts.end(), ts_bounds.first);
  auto min_idx = static_cast<uint32_t>(std::distance(ts.begin(), min_it));

  auto max_it = std::upper_bound(min_it, ts.end(), ts_bounds.second);
  auto max_idx = static_cast<uint32_t>(std::distance(ts.begin(), max_it));

  return std::make_pair(min_idx, max_idx);
}

std::vector<bool> FilterNonTsColumns(const TraceStorage* storage,
                                     const QueryConstraints& qc,
                                     sqlite3_value** argv,
                                     uint32_t min_idx,
                                     uint32_t max_idx) {
  const auto& slices = storage->slices();
  ptrdiff_t min_idx_ptr = static_cast<ptrdiff_t>(min_idx);
  ptrdiff_t max_idx_ptr = static_cast<ptrdiff_t>(max_idx);

  auto dist = static_cast<size_t>(max_idx - min_idx);
  std::vector<bool> filter(dist, true);
  for (size_t i = 0; i < qc.constraints().size(); i++) {
    const auto& cs = qc.constraints()[i];
    auto* v = argv[i];
    switch (cs.iColumn) {
      case SchedSliceTable::Column::kCpu: {
        auto it = slices.cpus().begin();
        FilterColumn(it + min_idx_ptr, it + max_idx_ptr, cs, v, &filter);
        break;
      }
      case SchedSliceTable::Column::kDuration: {
        auto it = slices.durations().begin();
        FilterColumn(it + min_idx_ptr, it + max_idx_ptr, cs, v, &filter);
        break;
      }
      case SchedSliceTable::Column::kUtid: {
        auto it = slices.utids().begin();
        FilterColumn(it + min_idx_ptr, it + max_idx_ptr, cs, v, &filter);
        break;
      }
    }
  }
  return filter;
}

bool HasOnlyTsConstraints(const QueryConstraints& qc) {
  auto fn = [](const QueryConstraints::Constraint& c) {
    return c.iColumn == SchedSliceTable::Column::kTimestamp;
  };
  return std::all_of(qc.constraints().begin(), qc.constraints().end(), fn);
}

bool IsTsOrdered(const QueryConstraints& qc) {
  return qc.order_by().size() == 0 ||
         (qc.order_by().size() == 1 &&
          qc.order_by()[0].iColumn == SchedSliceTable::Column::kTimestamp);
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

std::unique_ptr<Table::Cursor> SchedSliceTable::CreateCursor(
    const QueryConstraints& qc,
    sqlite3_value** argv) {
  auto ts_indices = FindTsIndices(storage_, GetTsBounds(qc, argv));
  auto min_idx = ts_indices.first;
  auto max_idx = ts_indices.second;

  if (HasOnlyTsConstraints(qc)) {
    if (IsTsOrdered(qc)) {
      bool desc = qc.order_by().size() == 1 && qc.order_by()[0].desc;
      return std::unique_ptr<Table::Cursor>(
          new IncrementCursor(storage_, min_idx, max_idx, desc));
    }
    return std::unique_ptr<Table::Cursor>(
        new SortedCursor(storage_, min_idx, max_idx, qc.order_by()));
  }

  std::vector<bool> filter =
      FilterNonTsColumns(storage_, qc, argv, min_idx, max_idx);
  if (IsTsOrdered(qc)) {
    bool desc = qc.order_by().size() == 1 && qc.order_by()[0].desc;
    return std::unique_ptr<Table::Cursor>(
        new FilterCursor(storage_, min_idx, max_idx, std::move(filter), desc));
  }
  return std::unique_ptr<Table::Cursor>(new SortedCursor(
      storage_, min_idx, max_idx, qc.order_by(), std::move(filter)));
}

int SchedSliceTable::BestIndex(const QueryConstraints& qc,
                               BestIndexInfo* info) {
  bool is_time_constrained =
      !qc.constraints().empty() && HasOnlyTsConstraints(qc);
  info->estimated_cost = is_time_constrained ? 10 : 10000;
  info->order_by_consumed = true;

  // We should be able to handle any constraint thrown at us.
  std::fill(info->omit.begin(), info->omit.end(), true);

  return SQLITE_OK;
}

SchedSliceTable::BaseCursor::BaseCursor(const TraceStorage* storage)
    : storage_(storage) {}
SchedSliceTable::BaseCursor::~BaseCursor() = default;

int SchedSliceTable::BaseCursor::Column(sqlite3_context* context, int N) {
  size_t row = RowIndex();
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

SchedSliceTable::IncrementCursor::IncrementCursor(const TraceStorage* storage,
                                                  uint32_t min_idx,
                                                  uint32_t max_idx,
                                                  bool desc)
    : BaseCursor(storage), min_idx_(min_idx), max_idx_(max_idx), desc_(desc) {}

int SchedSliceTable::IncrementCursor::Next() {
  offset_++;
  return SQLITE_OK;
}

uint32_t SchedSliceTable::IncrementCursor::RowIndex() {
  return desc_ ? max_idx_ - 1 - offset_ : min_idx_ + offset_;
}

int SchedSliceTable::IncrementCursor::Eof() {
  return offset_ >= (max_idx_ - min_idx_);
}

SchedSliceTable::FilterCursor::FilterCursor(const TraceStorage* storage,
                                            uint32_t min_idx,
                                            uint32_t max_idx,
                                            std::vector<bool> filter,
                                            bool desc)
    : BaseCursor(storage),
      min_idx_(min_idx),
      max_idx_(max_idx),
      filter_(std::move(filter)),
      desc_(desc) {
  PERFETTO_CHECK(max_idx - min_idx == filter_.size());
  FindNext();
}

int SchedSliceTable::FilterCursor::Next() {
  offset_++;
  FindNext();
  return SQLITE_OK;
}

void SchedSliceTable::FilterCursor::FindNext() {
  ptrdiff_t offset = static_cast<ptrdiff_t>(offset_);
  if (desc_) {
    auto it = std::find(filter_.rbegin() + offset, filter_.rend(), true);
    offset_ = static_cast<uint32_t>(std::distance(filter_.rbegin(), it));
  } else {
    auto it = std::find(filter_.begin() + offset, filter_.end(), true);
    offset_ = static_cast<uint32_t>(std::distance(filter_.begin(), it));
  }
}

uint32_t SchedSliceTable::FilterCursor::RowIndex() {
  return desc_ ? max_idx_ - 1 - offset_ : min_idx_ + offset_;
}

int SchedSliceTable::FilterCursor::Eof() {
  return offset_ >= (max_idx_ - min_idx_);
}

SchedSliceTable::SortedCursor::SortedCursor(
    const TraceStorage* storage,
    uint32_t min_idx,
    uint32_t max_idx,
    const std::vector<QueryConstraints::OrderBy>& ob)
    : BaseCursor(storage) {
  auto diff = static_cast<size_t>(max_idx - min_idx);
  sorted_rows_.resize(static_cast<size_t>(diff));

  std::iota(sorted_rows_.begin(), sorted_rows_.end(), min_idx);
  std::sort(sorted_rows_.begin(), sorted_rows_.end(),
            [this, &ob](uint32_t f, uint32_t s) {
              return CompareSlices(storage_, f, s, ob) < 0;
            });
}

SchedSliceTable::SortedCursor::SortedCursor(
    const TraceStorage* storage,
    uint32_t min_idx,
    uint32_t max_idx,
    const std::vector<QueryConstraints::OrderBy>& ob,
    const std::vector<bool>& filter)
    : BaseCursor(storage) {
  auto diff = static_cast<size_t>(max_idx - min_idx);
  PERFETTO_CHECK(diff == filter.size());

  auto set_bits = std::count(filter.begin(), filter.end(), true);
  sorted_rows_.resize(static_cast<size_t>(set_bits));

  auto it = std::find(filter.begin(), filter.end(), true);
  for (size_t i = 0; it != filter.end(); i++) {
    auto index = static_cast<uint32_t>(std::distance(filter.begin(), it));
    sorted_rows_[i] = min_idx + index;
    it = std::find(it + 1, filter.end(), true);
  }
  std::sort(sorted_rows_.begin(), sorted_rows_.end(),
            [this, &ob](uint32_t f, uint32_t s) {
              return CompareSlices(storage_, f, s, ob) < 0;
            });
}

int SchedSliceTable::SortedCursor::Next() {
  next_row_idx_++;
  return SQLITE_OK;
}

uint32_t SchedSliceTable::SortedCursor::RowIndex() {
  return sorted_rows_[next_row_idx_];
}

int SchedSliceTable::SortedCursor::Eof() {
  return next_row_idx_ >= sorted_rows_.size();
}

}  // namespace trace_processor
}  // namespace perfetto
