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

namespace perfetto {
namespace trace_processor {

namespace {
inline bool IsOpEq(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_EQ;
}

inline bool IsOpGe(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_GE;
}

inline bool IsOpGt(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_GT;
}

inline bool IsOpLe(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_LE;
}

inline bool IsOpLt(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_LT;
}

inline SchedSliceTable* AsTable(sqlite3_vtab* vtab) {
  return reinterpret_cast<SchedSliceTable*>(vtab);
}

template <size_t N = TraceStorage::kMaxCpus>
bool PopulateFilterBitmap(int op,
                          sqlite3_value* value,
                          std::bitset<N>* filter) {
  bool constraint_implemented = true;
  int64_t int_value = sqlite3_value_int64(value);
  if (IsOpGe(op) || IsOpGt(op)) {
    // If the operator is gt, then add one to the upper bound.
    int_value = IsOpGt(op) ? int_value + 1 : int_value;

    // Set to false all values less than |int_value|.
    size_t ub = static_cast<size_t>(std::max(0l, int_value));
    ub = std::min(ub, filter->size());
    for (size_t i = 0; i < ub; i++) {
      filter->set(i, false);
    }
  } else if (IsOpLe(op) || IsOpLt(op)) {
    // If the operator is lt, then minus one to the lower bound.
    int_value = IsOpLt(op) ? int_value - 1 : int_value;

    // Set to false all values greater than |int_value|.
    size_t lb = static_cast<size_t>(std::max(0l, int_value));
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

// Compares the slice at index |f| in |f_slices| for CPU |f_cpu| with the
// slice at index |s| in |s_slices| for CPU |s_cpu| on |column| in either
// ascending or descending mode depending on |desc|
// Returns -1 if the first slice is before the second in the ordering, 1 if
// the first slice is after the second and 0 if they are equal.
inline int CompareValuesForColumn(uint32_t f_cpu,
                                  const TraceStorage::SlicesPerCpu& f_slices,
                                  size_t f,
                                  uint32_t s_cpu,
                                  const TraceStorage::SlicesPerCpu& s_slices,
                                  size_t s,
                                  SchedSliceTable::Column column,
                                  bool desc) {
  switch (column) {
    case SchedSliceTable::Column::kTimestamp:
      return Compare(f_slices.start_ns()[f], s_slices.start_ns()[s], desc);
    case SchedSliceTable::Column::kDuration:
      return Compare(f_slices.durations()[f], s_slices.durations()[s], desc);
    case SchedSliceTable::Column::kCpu:
      return Compare(f_cpu, s_cpu, desc);
  }
}

// Creates a vector of indices into the given |slices| sorted by the ordering
// criteria given by |order_by|.
std::vector<uint32_t> CreateSortedIndexVector(
    uint32_t cpu,
    const TraceStorage::SlicesPerCpu& slices,
    const std::vector<SchedSliceTable::OrderBy>& order_by) {
  PERFETTO_CHECK(slices.slice_count() <= std::numeric_limits<uint32_t>::max());

  std::vector<uint32_t> indices;
  indices.resize(slices.slice_count());
  std::iota(indices.begin(), indices.end(), 0u);
  auto callback = [cpu, &order_by, &slices](uint32_t f, uint32_t s) {
    for (const auto& ob : order_by) {
      int value = CompareValuesForColumn(cpu, slices, f, cpu, slices, s,
                                         ob.column, ob.desc);
      if (value < 0)
        return true;
      else if (value > 0)
        return false;
    }
    return false;
  };
  std::sort(indices.begin(), indices.end(), callback);
  return indices;
}

}  // namespace

SchedSliceTable::SchedSliceTable(const TraceStorage* storage)
    : storage_(storage) {
  static_assert(offsetof(SchedSliceTable, base_) == 0,
                "SQLite base class must be first member of the table");
  memset(&base_, 0, sizeof(base_));
}

sqlite3_module SchedSliceTable::CreateModule() {
  sqlite3_module module;
  memset(&module, 0, sizeof(module));
  module.xConnect = [](sqlite3* db, void* raw_args, int, const char* const*,
                       sqlite3_vtab** tab, char**) {
    int res = sqlite3_declare_vtab(db,
                                   "CREATE TABLE sched_slices("
                                   "ts UNSIGNED BIG INT, "
                                   "cpu UNSIGNED INT, "
                                   "dur UNSIGNED BIG INT, "
                                   "PRIMARY KEY(cpu, ts)"
                                   ") WITHOUT ROWID;");
    if (res != SQLITE_OK)
      return res;
    TraceStorage* storage = static_cast<TraceStorage*>(raw_args);
    *tab = reinterpret_cast<sqlite3_vtab*>(new SchedSliceTable(storage));
    return SQLITE_OK;
  };
  module.xBestIndex = [](sqlite3_vtab* t, sqlite3_index_info* i) {
    return AsTable(t)->BestIndex(i);
  };
  module.xDisconnect = [](sqlite3_vtab* t) {
    delete AsTable(t);
    return SQLITE_OK;
  };
  module.xOpen = [](sqlite3_vtab* t, sqlite3_vtab_cursor** c) {
    return AsTable(t)->Open(c);
  };
  module.xClose = [](sqlite3_vtab_cursor* c) {
    delete AsCursor(c);
    return SQLITE_OK;
  };
  module.xFilter = [](sqlite3_vtab_cursor* c, int i, const char* s, int a,
                      sqlite3_value** v) {
    return AsCursor(c)->Filter(i, s, a, v);
  };
  module.xNext = [](sqlite3_vtab_cursor* c) { return AsCursor(c)->Next(); };
  module.xEof = [](sqlite3_vtab_cursor* c) { return AsCursor(c)->Eof(); };
  module.xColumn = [](sqlite3_vtab_cursor* c, sqlite3_context* a, int b) {
    return AsCursor(c)->Column(a, b);
  };
  return module;
}

int SchedSliceTable::Open(sqlite3_vtab_cursor** ppCursor) {
  *ppCursor =
      reinterpret_cast<sqlite3_vtab_cursor*>(new Cursor(this, storage_));
  return SQLITE_OK;
}

// Called at least once but possibly many times before filtering things and is
// the best time to keep track of constriants.
int SchedSliceTable::BestIndex(sqlite3_index_info* idx) {
  indexes_.emplace_back();
  IndexInfo* index = &indexes_.back();
  for (int i = 0; i < idx->nOrderBy; i++) {
    index->order_by.emplace_back();

    OrderBy* order = &index->order_by.back();
    order->column = static_cast<Column>(idx->aOrderBy[i].iColumn);
    order->desc = idx->aOrderBy[i].desc;
  }
  idx->orderByConsumed = true;

  for (int i = 0; i < idx->nConstraint; i++) {
    const auto& cs = idx->aConstraint[i];
    if (!cs.usable)
      continue;
    index->constraints.emplace_back(cs);

    // argvIndex is 1-based so use the current size of the vector.
    int argv_index = static_cast<int>(index->constraints.size());
    idx->aConstraintUsage[i].argvIndex = argv_index;
  }
  idx->idxNum = static_cast<int>(indexes_.size() - 1);

  return SQLITE_OK;
}

SchedSliceTable::Cursor::Cursor(SchedSliceTable* table,
                                const TraceStorage* storage)
    : table_(table), storage_(storage) {
  static_assert(offsetof(Cursor, base_) == 0,
                "SQLite base class must be first member of the cursor");
  memset(&base_, 0, sizeof(base_));
}

int SchedSliceTable::Cursor::Filter(int idxNum,
                                    const char* /* idxStr */,
                                    int argc,
                                    sqlite3_value** argv) {
  // Reset the filter state.
  const auto& index = table_->indexes_[static_cast<size_t>(idxNum)];
  PERFETTO_CHECK(index.constraints.size() == static_cast<size_t>(argc));

  filter_state_.reset(new FilterState(storage_, std::move(index.order_by),
                                      std::move(index.constraints), argv));

  table_->indexes_.clear();
  return SQLITE_OK;
}

int SchedSliceTable::Cursor::Next() {
  uint32_t cpu = filter_state_->next_cpu();
  auto* state = filter_state_->StateForCpu(cpu);

  // TODO(lalitm): maybe one day we may want to filter more efficiently. If so
  // update this method with filter logic.
  state->set_next_row_id_index(state->next_row_id_index() + 1);

  filter_state_->FindCpuWithNextSlice();
  return SQLITE_OK;
}

int SchedSliceTable::Cursor::Eof() {
  return !filter_state_->IsNextCpuValid();
}

int SchedSliceTable::Cursor::Column(sqlite3_context* context, int N) {
  if (!filter_state_->IsNextCpuValid())
    return SQLITE_ERROR;

  uint32_t cpu = filter_state_->next_cpu();
  size_t row = filter_state_->StateForCpu(cpu)->next_row_id();
  const auto& slices = storage_->SlicesForCpu(cpu);
  switch (N) {
    case Column::kTimestamp: {
      auto timestamp = static_cast<sqlite3_int64>(slices.start_ns()[row]);
      sqlite3_result_int64(context, timestamp);
      break;
    }
    case Column::kCpu: {
      sqlite3_result_int(context, static_cast<int>(cpu));
      break;
    }
    case Column::kDuration: {
      auto duration = static_cast<sqlite3_int64>(slices.durations()[row]);
      sqlite3_result_int64(context, duration);
      break;
    }
  }
  return SQLITE_OK;
}

int SchedSliceTable::Cursor::RowId(sqlite_int64* /* pRowid */) {
  return SQLITE_ERROR;
}

SchedSliceTable::Cursor::FilterState::FilterState(
    const TraceStorage* storage,
    std::vector<OrderBy> order_by,
    std::vector<Constraint> constraints,
    sqlite3_value** argv)
    : storage_(storage), order_by_(std::move(order_by)) {
  std::bitset<TraceStorage::kMaxCpus> cpu_filter;
  cpu_filter.set();

  for (size_t i = 0; i < constraints.size(); i++) {
    const auto& cs = constraints[i];
    switch (cs.iColumn) {
      case Column::kCpu:
        PopulateFilterBitmap(cs.op, argv[i], &cpu_filter);
        break;
    }
  }

  // First setup CPU filtering because the trace storage is indexed by CPU.
  for (uint32_t cpu = 0; cpu < TraceStorage::kMaxCpus; cpu++) {
    if (!cpu_filter.test(cpu))
      continue;

    PerCpuState* state = StateForCpu(cpu);

    // Create a sorted index vector based on the order by requirements.
    *state->sorted_row_ids() =
        CreateSortedIndexVector(cpu, storage_->SlicesForCpu(cpu), order_by_);
  }

  // Set the cpu index to be the first item to look at.
  FindCpuWithNextSlice();
}

void SchedSliceTable::Cursor::FilterState::FindCpuWithNextSlice() {
  next_cpu_ = TraceStorage::kMaxCpus;

  for (uint32_t cpu = 0; cpu < TraceStorage::kMaxCpus; cpu++) {
    const auto& cpu_state = per_cpu_state_[cpu];
    if (!cpu_state.IsNextRowIdIndexValid())
      continue;

    // The first CPU with a valid slice can be set to the next CPU.
    if (next_cpu_ == TraceStorage::kMaxCpus) {
      next_cpu_ = cpu;
      continue;
    }

    // If the current CPU is ordered before the current "next" CPU, then update
    // the cpu value.
    int cmp = CompareCpuToNextCpu(cpu);
    if (cmp < 0) {
      next_cpu_ = cpu;
    }
  }
}

int SchedSliceTable::Cursor::FilterState::CompareCpuToNextCpu(uint32_t cpu) {
  const auto& next_cpu_slices = storage_->SlicesForCpu(next_cpu_);
  size_t next_cpu_row = per_cpu_state_[next_cpu_].next_row_id();

  const auto& slices = storage_->SlicesForCpu(cpu);
  size_t row = per_cpu_state_[cpu].next_row_id();
  for (const auto& ob : order_by_) {
    int ret =
        CompareValuesForColumn(cpu, slices, row, next_cpu_, next_cpu_slices,
                               next_cpu_row, ob.column, ob.desc);
    if (ret != 0) {
      return ret;
    }
  }
  return 0;
}

}  // namespace trace_processor
}  // namespace perfetto
