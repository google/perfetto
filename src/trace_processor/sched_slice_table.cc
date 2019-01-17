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
      .AddNumericColumn("utid", &slices.utids())
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

}  // namespace trace_processor
}  // namespace perfetto
