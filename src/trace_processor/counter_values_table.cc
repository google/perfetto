/*
 * Copyright (C) 9 The Android Open Source Project
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

#include "src/trace_processor/counter_values_table.h"

namespace perfetto {
namespace trace_processor {

CounterValuesTable::CounterValuesTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void CounterValuesTable::RegisterTable(sqlite3* db,
                                       const TraceStorage* storage) {
  Table::Register<CounterValuesTable>(db, storage, "counter_values");
}

StorageSchema CounterValuesTable::CreateStorageSchema() {
  const auto& cs = storage_->counter_values();
  return StorageSchema::Builder()
      .AddColumn<IdColumn>("id", TableId::kCounterValues)
      .AddNumericColumn("counter_id", &cs.counter_ids())
      .AddOrderedNumericColumn("ts", &cs.timestamps())
      .AddNumericColumn("value", &cs.values())
      .AddNumericColumn("arg_set_id", &cs.arg_set_ids())
      .Build({"id"});
}

uint32_t CounterValuesTable::RowCount() {
  return storage_->counter_values().size();
}

int CounterValuesTable::BestIndex(const QueryConstraints& qc,
                                  BestIndexInfo* info) {
  info->estimated_cost = EstimateCost(qc);

  info->order_by_consumed = true;
  for (size_t i = 0; i < qc.constraints().size(); i++) {
    info->omit[i] = true;
  }

  return SQLITE_OK;
}

uint32_t CounterValuesTable::EstimateCost(const QueryConstraints& qc) {
  if (HasEqConstraint(qc, "counter_id"))
    return RowCount() / 100;
  return RowCount();
}

}  // namespace trace_processor
}  // namespace perfetto
