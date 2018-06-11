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

inline SchedSliceTable::Cursor* AsCursor(sqlite3_vtab_cursor* cursor) {
  return reinterpret_cast<SchedSliceTable::Cursor*>(cursor);
}
}  // namespace

SchedSliceTable::SchedSliceTable(const TraceStorage* storage)
    : storage_(storage) {
  static_assert(offsetof(SchedSliceTable, base_) == 0,
                "SQLite base class must be first member of the table");
  memset(&base_, 0, sizeof(base_));
}

int SchedSliceTable::Open(sqlite3_vtab_cursor** ppCursor) {
  *ppCursor =
      reinterpret_cast<sqlite3_vtab_cursor*>(new Cursor(this, storage_));
  return SQLITE_OK;
}

int SchedSliceTable::Close(sqlite3_vtab_cursor* cursor) {
  delete AsCursor(cursor);
  return SQLITE_OK;
}

// Called at least once but possibly many times before filtering things and is
// the best time to keep track of constriants.
int SchedSliceTable::BestIndex(sqlite3_index_info* idx) {
  bool external_ordering_required = false;
  for (int i = 0; i < idx->nOrderBy; i++) {
    if (idx->aOrderBy[i].iColumn != Column::kTimestamp ||
        idx->aOrderBy[i].desc) {
      // TODO(lalitm): support ordering by other fields.
      external_ordering_required = true;
      break;
    }
  }
  idx->orderByConsumed = !external_ordering_required;

  indexes_.emplace_back();
  idx->idxNum = static_cast<int>(indexes_.size());
  std::vector<Constraint>* constraints = &indexes_.back();
  for (int i = 0; i < idx->nConstraint; i++) {
    const auto& cs = idx->aConstraint[i];
    if (!cs.usable)
      continue;
    constraints->emplace_back(cs);
    idx->aConstraintUsage[i].argvIndex =
        static_cast<int>(constraints->size() - 1);
  }
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
  filter_state_ = {};

  const auto& constraints = table_->indexes_[static_cast<size_t>(idxNum)];
  PERFETTO_CHECK(constraints.size() == static_cast<size_t>(argc));
  for (size_t i = 0; i < constraints.size(); i++) {
    const auto& cs = constraints[i];
    switch (cs.iColumn) {
      case Column::kTimestamp:
        filter_state_.timestamp_constraints.Initialize(cs, argv[i]);
        break;
      case Column::kCpu:
        filter_state_.cpu_constraints.Initialize(cs, argv[i]);
        break;
    }
  }

  // First setup CPU filtering because the trace storage is indexed by CPU.
  for (uint32_t cpu = 0; cpu < TraceStorage::kMaxCpus; cpu++) {
    const auto& slices = storage_->SlicesForCpu(cpu);

    // Start by setting index out of bounds if filtering below doesn't
    // yield any results.
    PerCpuState* state = &filter_state_.per_cpu_state[cpu];
    state->index = slices.slice_count();

    if (!filter_state_.cpu_constraints.Matches(cpu))
      continue;

    // Filter on other constraints now.
    FindNextSliceForCpu(cpu, 0ul /* start_index */);
  }

  // Set the cpu index to be the first item to look at.
  FindNextSliceAmongCpus();

  table_->indexes_.clear();
  return SQLITE_OK;
}

int SchedSliceTable::Cursor::Next() {
  uint32_t cpu = static_cast<uint32_t>(filter_state_.next_slice_cpu);
  FindNextSliceForCpu(cpu, filter_state_.per_cpu_state[cpu].index + 1);
  FindNextSliceAmongCpus();
  return SQLITE_OK;
}

int SchedSliceTable::Cursor::Eof() {
  return filter_state_.next_slice_cpu >= filter_state_.per_cpu_state.size();
}

int SchedSliceTable::Cursor::Column(sqlite3_context* context, int N) {
  if (filter_state_.next_slice_cpu >= filter_state_.per_cpu_state.size()) {
    return SQLITE_ERROR;
  }
  uint32_t cpu_index = static_cast<uint32_t>(filter_state_.next_slice_cpu);
  const auto& state = filter_state_.per_cpu_state[cpu_index];
  const auto& slices = storage_->SlicesForCpu(cpu_index);
  switch (N) {
    case Column::kTimestamp: {
      auto timestamp =
          static_cast<sqlite3_int64>(slices.start_ns()[state.index]);
      sqlite3_result_int64(context, timestamp);
      break;
    }
    case Column::kCpu: {
      sqlite3_result_int(context, static_cast<int>(cpu_index));
      break;
    }
    case Column::kDuration: {
      auto duration =
          static_cast<sqlite3_int64>(slices.durations()[state.index]);
      sqlite3_result_int64(context, duration);
      break;
    }
  }
  return SQLITE_OK;
}

int SchedSliceTable::Cursor::RowId(sqlite_int64* /* pRowid */) {
  return SQLITE_ERROR;
}

void SchedSliceTable::Cursor::FindNextSliceForCpu(uint32_t cpu,
                                                  size_t start_index) {
  auto* state = &filter_state_.per_cpu_state[cpu];
  const auto& slices = storage_->SlicesForCpu(cpu);

  // Store the position we should start filtering from before setting
  // the index out of bounds in case the loop doesn't match anything.
  state->index = slices.slice_count();

  for (size_t i = start_index; i < slices.slice_count(); i++) {
    if (filter_state_.timestamp_constraints.Matches(slices.start_ns()[i])) {
      state->index = i;
      break;
    }
  }
}

void SchedSliceTable::Cursor::FindNextSliceAmongCpus() {
  filter_state_.next_slice_cpu = filter_state_.per_cpu_state.size();

  uint64_t min_timestamp = std::numeric_limits<uint64_t>::max();
  for (uint32_t i = 0; i < filter_state_.per_cpu_state.size(); i++) {
    const auto& cpu_state = filter_state_.per_cpu_state[i];
    const auto& slices = storage_->SlicesForCpu(i);
    if (cpu_state.index >= slices.slice_count())
      continue;

    // TODO(lalitm): handle sorting by things other than timestamp.
    uint64_t cur_timestamp = slices.start_ns()[cpu_state.index];
    if (cur_timestamp < min_timestamp) {
      min_timestamp = cur_timestamp;
      filter_state_.next_slice_cpu = i;
    }
  }
}

template <typename T>
bool SchedSliceTable::Cursor::NumericConstraints<T>::Initialize(
    const Constraint& cs,
    sqlite3_value* value) {
  bool is_integral = std::is_integral<T>::value;
  PERFETTO_DCHECK(is_integral ? sqlite3_value_type(value) == SQLITE_INTEGER
                              : sqlite3_value_type(value) == SQLITE_FLOAT);
  bool constraint_implemented = true;
  T const_value = static_cast<T>(is_integral ? sqlite3_value_int64(value)
                                             : sqlite3_value_double(value));
  if (IsOpGe(cs.op) || IsOpGt(cs.op)) {
    min_value = const_value;
    min_equals = IsOpGt(cs.op);
  } else if (IsOpLe(cs.op) || IsOpLt(cs.op)) {
    max_value = const_value;
    max_equals = IsOpLt(cs.op);
  } else if (IsOpEq(cs.op)) {
    max_value = const_value;
    max_equals = true;
    min_value = const_value;
    min_equals = true;
  } else {
    constraint_implemented = false;
  }
  return constraint_implemented;
}

}  // namespace trace_processor
}  // namespace perfetto
