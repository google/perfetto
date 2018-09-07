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

SchedSliceTable::SchedSliceTable(const TraceStorage* storage)
    : storage_(storage) {}

void SchedSliceTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<SchedSliceTable>(db, storage,
                                   "CREATE TABLE sched("
                                   "quantum HIDDEN BIG INT, "
                                   "ts_lower_bound HIDDEN BIG INT, "
                                   "ts UNSIGNED BIG INT, "
                                   "cpu UNSIGNED INT, "
                                   "dur UNSIGNED BIG INT, "
                                   "quantized_group UNSIGNED BIG INT, "
                                   "utid UNSIGNED INT,"
                                   "cycles UNSIGNED BIG INT,"
                                   "PRIMARY KEY(cpu, ts)"
                                   ") WITHOUT ROWID;");
}

std::unique_ptr<Table::Cursor> SchedSliceTable::CreateCursor() {
  return std::unique_ptr<Table::Cursor>(new Cursor(storage_));
}

int SchedSliceTable::BestIndex(const QueryConstraints& qc,
                               BestIndexInfo* info) {
  // Omit SQLite constraint checks on the hidden columns, so the client can
  // write queries of the form "quantum == x" and "ts_lower_bound == x".
  // Disallow any other constraint on these columns.
  for (size_t i = 0; i < qc.constraints().size(); i++) {
    const auto& cs = qc.constraints()[i];
    if (cs.iColumn == Column::kTimestampLowerBound ||
        cs.iColumn == Column::kQuantizedGroup) {
      if (!IsOpEq(cs.op))
        return SQLITE_CONSTRAINT_FUNCTION;
      info->omit[i] = true;
    }
  }

  bool is_quantized_group_order_desc = false;
  bool is_duration_timestamp_order = false;
  for (const auto& ob : qc.order_by()) {
    switch (ob.iColumn) {
      case Column::kQuantizedGroup:
        if (ob.desc)
          is_quantized_group_order_desc = true;
        break;
      case Column::kTimestamp:
      case Column::kDuration:
        is_duration_timestamp_order = true;
        break;
      case Column::kQuantum:
      case Column::kCpu:
        break;
    }
  }

  bool has_quantum_constraint = false;
  for (const auto& cs : qc.constraints()) {
    if (cs.iColumn == Column::kQuantum)
      has_quantum_constraint = true;
  }

  // If a quantum constraint is present, we don't support native ordering by
  // time related parameters or by quantized group in descending order.
  bool needs_sqlite_orderby =
      has_quantum_constraint &&
      (is_duration_timestamp_order || is_quantized_group_order_desc);

  info->order_by_consumed = !needs_sqlite_orderby;

  return SQLITE_OK;
}

int SchedSliceTable::FindFunction(const char* name,
                                  FindFunctionFn fn,
                                  void** args) {
  // Add an identity match function to prevent throwing an exception when
  // matching on the quantum column.
  if (strcmp(name, "match") == 0) {
    *fn = [](sqlite3_context* ctx, int n, sqlite3_value** v) {
      PERFETTO_DCHECK(n == 2 && sqlite3_value_type(v[0]) == SQLITE_INTEGER);
      sqlite3_result_int64(ctx, sqlite3_value_int64(v[0]));
    };
    *args = nullptr;
    return 1;
  }
  return 0;
}

SchedSliceTable::Cursor::Cursor(const TraceStorage* storage)
    : storage_(storage) {}

int SchedSliceTable::Cursor::Filter(const QueryConstraints& qc,
                                    sqlite3_value** argv) {
  filter_state_.reset(new FilterState(storage_, qc, argv));
  return SQLITE_OK;
}

int SchedSliceTable::Cursor::Next() {
  auto* state = filter_state_->StateForCpu(filter_state_->next_cpu());
  state->FindNextSlice();
  filter_state_->FindCpuWithNextSlice();
  return SQLITE_OK;
}

int SchedSliceTable::Cursor::Eof() {
  return !filter_state_->IsNextCpuValid();
}

int SchedSliceTable::Cursor::Column(sqlite3_context* context, int N) {
  if (!filter_state_->IsNextCpuValid())
    return SQLITE_ERROR;

  uint64_t quantum = filter_state_->quantum();
  uint32_t cpu = filter_state_->next_cpu();
  const auto* state = filter_state_->StateForCpu(cpu);
  size_t row = state->next_row_id();
  const auto& slices = storage_->SlicesForCpu(cpu);
  switch (N) {
    case Column::kTimestamp: {
      auto timestamp = state->next_timestamp();
      sqlite3_result_int64(context, static_cast<sqlite3_int64>(timestamp));
      break;
    }
    case Column::kCpu: {
      sqlite3_result_int(context, static_cast<int>(cpu));
      break;
    }
    case Column::kDuration: {
      uint64_t duration;
      if (quantum == 0) {
        duration = slices.durations()[row];
      } else {
        uint64_t start_quantised_group = state->next_timestamp() / quantum;
        uint64_t end = slices.start_ns()[row] + slices.durations()[row];
        uint64_t next_group_start = (start_quantised_group + 1) * quantum;

        // Compute the minimum of the start of the next group boundary and the
        // end of this slice.
        uint64_t min_slice_end = std::min<uint64_t>(end, next_group_start);
        duration = min_slice_end - state->next_timestamp();
      }
      sqlite3_result_int64(context, static_cast<sqlite3_int64>(duration));
      break;
    }
    case Column::kQuantizedGroup: {
      auto group = quantum == 0 ? state->next_timestamp()
                                : state->next_timestamp() / quantum;
      sqlite3_result_int64(context, static_cast<sqlite3_int64>(group));
      break;
    }
    case Column::kQuantum: {
      sqlite3_result_int64(context, static_cast<sqlite3_int64>(quantum));
      break;
    }
    case Column::kUtid: {
      sqlite3_result_int64(context, slices.utids()[row]);
      break;
    }
    case Column::kCycles: {
      sqlite3_result_int64(context,
                           static_cast<sqlite3_int64>(slices.cycles()[row]));
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
  std::bitset<base::kMaxCpus> cpu_filter;
  cpu_filter.set();

  uint64_t min_ts = 0;
  uint64_t max_ts = std::numeric_limits<uint64_t>::max();
  uint64_t ts_lower_bound = 0;

  for (size_t i = 0; i < query_constraints.constraints().size(); i++) {
    const auto& cs = query_constraints.constraints()[i];
    switch (cs.iColumn) {
      case Column::kCpu:
        PopulateFilterBitmap(cs.op, argv[i], &cpu_filter);
        break;
      case Column::kQuantum:
        quantum_ = static_cast<uint64_t>(sqlite3_value_int64(argv[i]));
        break;
      case Column::kTimestampLowerBound:
        ts_lower_bound = static_cast<uint64_t>(sqlite3_value_int64(argv[i]));
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

  // If the query specifies a lower bound on ts, find that bound across all
  // CPUs involved in the query and turn that into a min_ts constraint.
  // ts_lower_bound is defined as the largest timestamp < X, or if none, the
  // smallest timestamp >= X.
  if (ts_lower_bound > 0) {
    uint64_t largest_ts_before = 0;
    uint64_t smallest_ts_after = std::numeric_limits<uint64_t>::max();
    for (uint32_t cpu = 0; cpu < base::kMaxCpus; cpu++) {
      if (!cpu_filter.test(cpu))
        continue;
      const auto& start_ns = storage_->SlicesForCpu(cpu).start_ns();
      // std::lower_bound will find the first timestamp >= |ts_lower_bound|.
      // From there we need to move one back, if possible.
      auto it =
          std::lower_bound(start_ns.begin(), start_ns.end(), ts_lower_bound);
      if (std::distance(start_ns.begin(), it) > 0)
        it--;
      if (it == start_ns.end())
        continue;
      if (*it < ts_lower_bound) {
        largest_ts_before = std::max(largest_ts_before, *it);
      } else {
        smallest_ts_after = std::min(smallest_ts_after, *it);
      }
    }
    uint64_t lower_bound = std::min(largest_ts_before, smallest_ts_after);
    min_ts = std::max(min_ts, lower_bound);
  }  // if (ts_lower_bound)

  // Setup CPU filtering because the trace storage is indexed by CPU.
  for (uint32_t cpu = 0; cpu < base::kMaxCpus; cpu++) {
    if (!cpu_filter.test(cpu))
      continue;
    StateForCpu(cpu)->Initialize(
        cpu, storage_, quantum_,
        CreateSortedIndexVectorForCpu(cpu, min_ts, max_ts));
  }

  // Set the cpu index to be the first item to look at.
  FindCpuWithNextSlice();
}

void SchedSliceTable::FilterState::FindCpuWithNextSlice() {
  next_cpu_ = base::kMaxCpus;

  for (uint32_t cpu = 0; cpu < base::kMaxCpus; cpu++) {
    const auto& cpu_state = per_cpu_state_[cpu];
    if (!cpu_state.IsNextRowIdIndexValid())
      continue;

    // The first CPU with a valid slice can be set to the next CPU.
    if (next_cpu_ == base::kMaxCpus) {
      next_cpu_ = cpu;
      continue;
    }

    // If the current CPU is ordered before the current "next" CPU, then update
    // the cpu value.
    int cmp = CompareCpuToNextCpu(cpu);
    if (cmp < 0)
      next_cpu_ = cpu;
  }
}

int SchedSliceTable::FilterState::CompareCpuToNextCpu(uint32_t cpu) {
  size_t next_row = per_cpu_state_[next_cpu_].next_row_id();
  size_t row = per_cpu_state_[cpu].next_row_id();
  return CompareSlices(cpu, row, next_cpu_, next_row);
}

std::vector<uint32_t>
SchedSliceTable::FilterState::CreateSortedIndexVectorForCpu(uint32_t cpu,
                                                            uint64_t min_ts,
                                                            uint64_t max_ts) {
  const auto& slices = storage_->SlicesForCpu(cpu);
  const auto& start_ns = slices.start_ns();
  PERFETTO_CHECK(slices.slice_count() <= std::numeric_limits<uint32_t>::max());

  auto min_it = std::lower_bound(start_ns.begin(), start_ns.end(), min_ts);
  auto max_it = std::upper_bound(min_it, start_ns.end(), max_ts);
  ptrdiff_t dist = std::distance(min_it, max_it);
  PERFETTO_CHECK(dist >= 0 && static_cast<size_t>(dist) <= start_ns.size());

  std::vector<uint32_t> indices(static_cast<size_t>(dist));

  // Fill |indices| with the consecutive row numbers affected by the filtering.
  std::iota(indices.begin(), indices.end(),
            std::distance(start_ns.begin(), min_it));

  // In other cases, sort by the given criteria.
  std::sort(indices.begin(), indices.end(),
            [this, cpu](uint32_t f, uint32_t s) {
              return CompareSlices(cpu, f, cpu, s) < 0;
            });
  return indices;
}

int SchedSliceTable::FilterState::CompareSlices(uint32_t f_cpu,
                                                size_t f_idx,
                                                uint32_t s_cpu,
                                                size_t s_idx) {
  for (const auto& ob : order_by_) {
    int c = CompareSlicesOnColumn(f_cpu, f_idx, s_cpu, s_idx, ob);
    if (c != 0)
      return c;
  }
  return 0;
}

int SchedSliceTable::FilterState::CompareSlicesOnColumn(
    uint32_t f_cpu,
    size_t f_idx,
    uint32_t s_cpu,
    size_t s_idx,
    const QueryConstraints::OrderBy& ob) {
  const auto& f_sl = storage_->SlicesForCpu(f_cpu);
  const auto& s_sl = storage_->SlicesForCpu(s_cpu);
  switch (ob.iColumn) {
    case SchedSliceTable::Column::kQuantum:
    case SchedSliceTable::Column::kTimestampLowerBound:
      PERFETTO_CHECK(false);
    case SchedSliceTable::Column::kTimestamp:
      return Compare(f_sl.start_ns()[f_idx], s_sl.start_ns()[s_idx], ob.desc);
    case SchedSliceTable::Column::kDuration:
      return Compare(f_sl.durations()[f_idx], s_sl.durations()[s_idx], ob.desc);
    case SchedSliceTable::Column::kCpu:
      return Compare(f_cpu, s_cpu, ob.desc);
    case SchedSliceTable::Column::kUtid:
      return Compare(f_sl.utids()[f_idx], s_sl.utids()[s_idx], ob.desc);
    case SchedSliceTable::Column::kCycles:
      return Compare(f_sl.cycles()[f_idx], s_sl.cycles()[s_idx], ob.desc);
    case SchedSliceTable::Column::kQuantizedGroup: {
      // We don't support sorting in descending order on quantized group when
      // we have a non-zero quantum.
      PERFETTO_CHECK(!ob.desc || quantum_ == 0);

      uint64_t f_timestamp = StateForCpu(f_cpu)->next_timestamp();
      uint64_t s_timestamp = StateForCpu(s_cpu)->next_timestamp();

      uint64_t f_group = quantum_ == 0 ? f_timestamp : f_timestamp / quantum_;
      uint64_t s_group = quantum_ == 0 ? s_timestamp : s_timestamp / quantum_;
      return Compare(f_group, s_group, ob.desc);
    }
  }
  PERFETTO_FATAL("Unexpected column %d", ob.iColumn);
}

void SchedSliceTable::PerCpuState::Initialize(
    uint32_t cpu,
    const TraceStorage* storage,
    uint64_t quantum,
    std::vector<uint32_t> sorted_row_ids) {
  cpu_ = cpu;
  storage_ = storage;
  quantum_ = quantum;
  sorted_row_ids_ = std::move(sorted_row_ids);
  UpdateNextTimestampForNextRow();
}

void SchedSliceTable::PerCpuState::FindNextSlice() {
  PERFETTO_DCHECK(next_timestamp_ != 0);

  const auto& slices = Slices();
  if (quantum_ == 0) {
    next_row_id_index_++;
    UpdateNextTimestampForNextRow();
    return;
  }

  uint64_t start_group = next_timestamp_ / quantum_;
  uint64_t end_slice =
      slices.start_ns()[next_row_id()] + slices.durations()[next_row_id()];
  uint64_t next_group_start = (start_group + 1) * quantum_;

  if (next_group_start >= end_slice) {
    next_row_id_index_++;
    UpdateNextTimestampForNextRow();
  } else {
    next_timestamp_ = next_group_start;
  }
}

void SchedSliceTable::PerCpuState::UpdateNextTimestampForNextRow() {
  next_timestamp_ =
      IsNextRowIdIndexValid() ? Slices().start_ns()[next_row_id()] : 0;
}

}  // namespace trace_processor
}  // namespace perfetto
