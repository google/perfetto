/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/counter_definitions_table.h"

#include <string>

#include "src/trace_processor/storage_columns.h"

namespace perfetto {
namespace trace_processor {

CounterDefinitionsTable::CounterDefinitionsTable(sqlite3*,
                                                 const TraceStorage* storage)
    : storage_(storage) {}

void CounterDefinitionsTable::RegisterTable(sqlite3* db,
                                            const TraceStorage* storage) {
  Table::Register<CounterDefinitionsTable>(db, storage, "counter_definitions");
}

StorageSchema CounterDefinitionsTable::CreateStorageSchema() {
  const auto& cs = storage_->counter_definitions();
  return StorageSchema::Builder()
      .AddGenericNumericColumn("counter_id", RowAccessor())
      .AddStringColumn("name", &cs.name_ids(), &storage_->string_pool())
      .AddColumn<RefColumn>("ref", &cs.refs(), &cs.types(), storage_)
      .AddStringColumn("ref_type", &cs.types(), &GetRefTypeStringMap())
      .Build({"counter_id"});
}

uint32_t CounterDefinitionsTable::RowCount() {
  return storage_->counter_definitions().size();
}

int CounterDefinitionsTable::BestIndex(const QueryConstraints& qc,
                                       BestIndexInfo* info) {
  info->estimated_cost = EstimateCost(qc);

  // Only the string columns are handled by SQLite
  size_t name_index = schema().ColumnIndexFromName("name");
  size_t ref_type_index = schema().ColumnIndexFromName("ref_type");
  info->order_by_consumed = true;
  for (size_t i = 0; i < qc.constraints().size(); i++) {
    auto col = static_cast<size_t>(qc.constraints()[i].iColumn);
    info->omit[i] = col != name_index && col != ref_type_index;
  }

  return SQLITE_OK;
}

uint32_t CounterDefinitionsTable::EstimateCost(const QueryConstraints& qc) {
  // If there is a constraint on the counter id, we can efficiently filter
  // to a single row.
  if (HasEqConstraint(qc, "counter_id"))
    return 1;

  auto eq_name = HasEqConstraint(qc, "name");
  auto eq_ref = HasEqConstraint(qc, "ref");
  auto eq_ref_type = HasEqConstraint(qc, "ref_type");

  // If there is a constraint on all three columns, we are going to only return
  // exaclty one row for sure so make the cost 1.
  if (eq_name && eq_ref && eq_ref_type)
    return 1;
  else if (eq_name && eq_ref)
    return 10;
  else if (eq_name)
    return 100;
  return RowCount();
}

}  // namespace trace_processor
}  // namespace perfetto
