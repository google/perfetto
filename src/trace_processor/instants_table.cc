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

#include "src/trace_processor/instants_table.h"

#include "src/trace_processor/counters_table.h"

namespace perfetto {
namespace trace_processor {

InstantsTable::InstantsTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {
  ref_types_.resize(RefType::kRefMax);
  ref_types_[RefType::kRefNoRef] = "";
  ref_types_[RefType::kRefUtid] = "utid";
  ref_types_[RefType::kRefCpuId] = "cpu";
  ref_types_[RefType::kRefIrq] = "irq";
  ref_types_[RefType::kRefSoftIrq] = "softirq";
  ref_types_[RefType::kRefUpid] = "upid";
};

void InstantsTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<InstantsTable>(db, storage, "instants");
}

StorageSchema InstantsTable::CreateStorageSchema() {
  const auto& instants = storage_->instants();
  return StorageSchema::Builder()
      .AddColumn<IdColumn>("id", TableId::kInstants)
      .AddOrderedNumericColumn("ts", &instants.timestamps())
      .AddStringColumn("name", &instants.name_ids(), &storage_->string_pool())
      .AddNumericColumn("value", &instants.values())
      .AddColumn<CountersTable::RefColumn>("ref", &instants.refs(),
                                           &instants.types(), storage_)
      .AddStringColumn("ref_type", &instants.types(), &ref_types_)
      .AddNumericColumn("arg_set_id", &instants.arg_set_ids())
      .Build({"name", "ts", "ref"});
}

uint32_t InstantsTable::RowCount() {
  return static_cast<uint32_t>(storage_->instants().instant_count());
}

int InstantsTable::BestIndex(const QueryConstraints& qc, BestIndexInfo* info) {
  info->estimated_cost =
      static_cast<uint32_t>(storage_->instants().instant_count());

  // Only the string columns are handled by SQLite
  info->order_by_consumed = true;
  size_t name_index = schema().ColumnIndexFromName("name");
  size_t ref_type_index = schema().ColumnIndexFromName("ref_type");
  for (size_t i = 0; i < qc.constraints().size(); i++) {
    info->omit[i] =
        qc.constraints()[i].iColumn != static_cast<int>(name_index) &&
        qc.constraints()[i].iColumn != static_cast<int>(ref_type_index);
  }

  return SQLITE_OK;
}
}  // namespace trace_processor
}  // namespace perfetto
