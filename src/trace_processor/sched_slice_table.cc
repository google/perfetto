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

namespace perfetto {
namespace trace_processor {

SchedSliceTable::SchedSliceTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void SchedSliceTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<SchedSliceTable>(db, storage, "sched");
}

StorageSchema SchedSliceTable::CreateStorageSchema() {
  const auto& slices = storage_->slices();
  return StorageSchema::Builder()
      .AddOrderedNumericColumn("ts", &slices.start_ns())
      .AddNumericColumn("cpu", &slices.cpus())
      .AddNumericColumn("dur", &slices.durations())
      .AddColumn<TsEndColumn>("ts_end", &slices.start_ns(), &slices.durations())
      .AddNumericColumn("utid", &slices.utids(), &slices.rows_for_utids())
      .AddColumn<EndStateColumn>("end_state", &slices.end_state())
      .AddNumericColumn("priority", &slices.priorities())
      .AddColumn<IdColumn>("row_id", TableId::kSched)
      .Build({"cpu", "ts"});
}

uint32_t SchedSliceTable::RowCount() {
  return static_cast<uint32_t>(storage_->slices().slice_count());
}

int SchedSliceTable::BestIndex(const QueryConstraints& qc,
                               BestIndexInfo* info) {
  info->estimated_cost = EstimateQueryCost(qc);

  // We should be able to handle any constraint and any order by clause given
  // to us.
  info->order_by_consumed = true;
  std::fill(info->omit.begin(), info->omit.end(), true);

  return SQLITE_OK;
}

uint32_t SchedSliceTable::EstimateQueryCost(const QueryConstraints& qc) {
  const auto& cs = qc.constraints();

  size_t ts_idx = schema().ColumnIndexFromName("ts");
  auto has_ts_column = [ts_idx](const QueryConstraints::Constraint& c) {
    return c.iColumn == static_cast<int>(ts_idx);
  };
  bool has_time_constraint = std::any_of(cs.begin(), cs.end(), has_ts_column);
  if (has_time_constraint) {
    // If there is a constraint on ts, we can do queries very fast (O(log n))
    // so always make this preferred if available.
    return 10;
  }

  size_t utid_idx = schema().ColumnIndexFromName("utid");
  auto has_utid_eq_cs = [utid_idx](const QueryConstraints::Constraint& c) {
    return c.iColumn == static_cast<int>(utid_idx) &&
           sqlite_utils::IsOpEq(c.op);
  };
  bool has_utid_eq = std::any_of(cs.begin(), cs.end(), has_utid_eq_cs);
  if (has_utid_eq) {
    // The other column which is often joined on is utid. Sometimes, doing
    // nested subqueries on the thread table is faster but with some queries,
    // it's actually better to do subqueries on this table. Estimate the cost
    // of filtering on utid equality constraint by dividing the number of slices
    // by the number of threads.
    return RowCount() / storage_->thread_count();
  }

  // If we get to this point, we do not have any special filter logic so
  // simply return the number of rows.
  return RowCount();
}

SchedSliceTable::EndStateColumn::EndStateColumn(
    std::string col_name,
    const std::deque<ftrace_utils::TaskState>* deque)
    : StorageColumn(col_name, false), deque_(deque) {
  for (uint16_t i = 0; i < state_strings_.size(); i++) {
    state_strings_[i] = ftrace_utils::TaskState(i).ToString();
  }
}
SchedSliceTable::EndStateColumn::~EndStateColumn() = default;

void SchedSliceTable::EndStateColumn::ReportResult(sqlite3_context* ctx,
                                                   uint32_t row) const {
  const auto& state = (*deque_)[row];
  if (state.is_valid()) {
    PERFETTO_CHECK(state.raw_state() < state_strings_.size());
    sqlite3_result_text(ctx, state_strings_[state.raw_state()].data(), -1,
                        sqlite_utils::kSqliteStatic);
  } else {
    sqlite3_result_null(ctx);
  }
}

void SchedSliceTable::EndStateColumn::Filter(int op,
                                             sqlite3_value* value,
                                             FilteredRowIndex* index) const {
  switch (op) {
    case SQLITE_INDEX_CONSTRAINT_ISNULL:
    case SQLITE_INDEX_CONSTRAINT_ISNOTNULL: {
      bool non_nulls = op == SQLITE_INDEX_CONSTRAINT_ISNOTNULL;
      index->FilterRows([this, non_nulls](uint32_t row) {
        const auto& state = (*deque_)[row];
        return state.is_valid() == non_nulls;
      });
      break;
    }
    case SQLITE_INDEX_CONSTRAINT_EQ:
    case SQLITE_INDEX_CONSTRAINT_NE:
    case SQLITE_INDEX_CONSTRAINT_MATCH:
      FilterOnState(op, value, index);
      break;
    default:
      index->set_error("Unsupported op given to filter on end_state");
      break;
  }
}

void SchedSliceTable::EndStateColumn::FilterOnState(
    int op,
    sqlite3_value* value,
    FilteredRowIndex* index) const {
  if (sqlite3_value_type(value) != SQLITE_TEXT) {
    index->set_error("end_state can only be filtered using strings");
    return;
  }

  const char* str = reinterpret_cast<const char*>(sqlite3_value_text(value));
  ftrace_utils::TaskState compare(str);
  if (!compare.is_valid()) {
    index->set_error("Invalid end_state string given to filter");
    return;
  }

  uint16_t raw_state = compare.raw_state();
  if (op == SQLITE_INDEX_CONSTRAINT_EQ) {
    index->FilterRows([this, raw_state](uint32_t row) {
      const auto& state = (*deque_)[row];
      return state.is_valid() && state.raw_state() == raw_state;
    });
  } else if (op == SQLITE_INDEX_CONSTRAINT_NE) {
    index->FilterRows([this, raw_state](uint32_t row) {
      const auto& state = (*deque_)[row];
      return state.is_valid() && state.raw_state() != raw_state;
    });
  } else if (op == SQLITE_INDEX_CONSTRAINT_MATCH) {
    index->FilterRows([this, compare](uint32_t row) {
      const auto& state = (*deque_)[row];
      if (!state.is_valid())
        return false;
      return (state.raw_state() & compare.raw_state()) == compare.raw_state();
    });
  } else {
    PERFETTO_FATAL("Should never reach this state");
  }
}

StorageColumn::Comparator SchedSliceTable::EndStateColumn::Sort(
    const QueryConstraints::OrderBy& ob) const {
  if (ob.desc) {
    return [this](uint32_t f, uint32_t s) {
      const auto& a = (*deque_)[f];
      const auto& b = (*deque_)[s];
      if (!a.is_valid()) {
        return !b.is_valid() ? 0 : 1;
      } else if (!b.is_valid()) {
        return -1;
      }
      return sqlite_utils::CompareValuesAsc(a.raw_state(), b.raw_state());
    };
  }
  return [this](uint32_t f, uint32_t s) {
    const auto& a = (*deque_)[f];
    const auto& b = (*deque_)[s];
    if (!a.is_valid()) {
      return !b.is_valid() ? 0 : -1;
    } else if (!b.is_valid()) {
      return 1;
    }
    return sqlite_utils::CompareValuesAsc(a.raw_state(), b.raw_state());
  };
}

Table::ColumnType SchedSliceTable::EndStateColumn::GetType() const {
  return Table::ColumnType::kString;
}

}  // namespace trace_processor
}  // namespace perfetto
