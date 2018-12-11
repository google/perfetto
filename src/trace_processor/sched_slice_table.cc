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

Table::Schema SchedSliceTable::CreateSchema(int, const char* const*) {
  const auto& slices = storage_->slices();
  std::unique_ptr<StorageColumn> cols[] = {
      NumericColumnPtr("ts", &slices.start_ns(), false /* hidden */,
                       true /* ordered */),
      NumericColumnPtr("cpu", &slices.cpus()),
      NumericColumnPtr("dur", &slices.durations()),
      TsEndPtr("ts_end", &slices.start_ns(), &slices.durations()),
      NumericColumnPtr("utid", &slices.utids())};
  schema_ = StorageSchema({
      std::make_move_iterator(std::begin(cols)),
      std::make_move_iterator(std::end(cols)),
  });
  return schema_.ToTableSchema({"cpu", "ts"});
}

std::unique_ptr<Table::Cursor> SchedSliceTable::CreateCursor(
    const QueryConstraints& qc,
    sqlite3_value** argv) {
  uint32_t count = static_cast<uint32_t>(storage_->slices().slice_count());
  auto it = CreateBestRowIteratorForGenericSchema(count, qc, argv);
  return std::unique_ptr<Table::Cursor>(
      new Cursor(std::move(it), schema_.mutable_columns()));
}

int SchedSliceTable::BestIndex(const QueryConstraints& qc,
                               BestIndexInfo* info) {
  const auto& cs = qc.constraints();
  size_t ts_idx = schema_.ColumnIndexFromName("ts");
  auto has_ts_column = [ts_idx](const QueryConstraints::Constraint& c) {
    return c.iColumn == static_cast<int>(ts_idx);
  };
  bool has_time_constraint = std::any_of(cs.begin(), cs.end(), has_ts_column);
  info->estimated_cost = has_time_constraint ? 10 : 10000;

  // We should be able to handle any constraint and any order by clause given
  // to us.
  info->order_by_consumed = true;
  std::fill(info->omit.begin(), info->omit.end(), true);

  return SQLITE_OK;
}

}  // namespace trace_processor
}  // namespace perfetto
