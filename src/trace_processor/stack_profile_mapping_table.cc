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

#include "src/trace_processor/stack_profile_mapping_table.h"

namespace perfetto {
namespace trace_processor {

StackProfileMappingTable::StackProfileMappingTable(sqlite3*,
                                                   const TraceStorage* storage)
    : storage_(storage) {}

void StackProfileMappingTable::RegisterTable(sqlite3* db,
                                             const TraceStorage* storage) {
  SqliteTable::Register<StackProfileMappingTable>(db, storage,
                                                  "stack_profile_mapping");
}

StorageSchema StackProfileMappingTable::CreateStorageSchema() {
  const auto& mappings = storage_->stack_profile_mappings();
  return StorageSchema::Builder()
      .AddGenericNumericColumn("id", RowAccessor())
      .AddStringColumn("build_id", &mappings.build_ids(),
                       &storage_->string_pool())
      .AddNumericColumn("exact_offset", &mappings.exact_offsets())
      .AddNumericColumn("start_offset", &mappings.start_offsets())
      .AddNumericColumn("start", &mappings.starts())
      .AddNumericColumn("end", &mappings.ends())
      .AddNumericColumn("load_bias", &mappings.load_biases())
      .AddStringColumn("name", &mappings.names(), &storage_->string_pool())
      .Build({"id"});
}

uint32_t StackProfileMappingTable::RowCount() {
  return storage_->stack_profile_mappings().size();
}

int StackProfileMappingTable::BestIndex(const QueryConstraints& qc,
                                        BestIndexInfo* info) {
  info->order_by_consumed = true;
  info->estimated_cost = HasEqConstraint(qc, "id") ? 1 : RowCount();
  return SQLITE_OK;
}

}  // namespace trace_processor
}  // namespace perfetto
