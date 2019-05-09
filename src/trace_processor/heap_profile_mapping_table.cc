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

#include "src/trace_processor/heap_profile_mapping_table.h"

namespace perfetto {
namespace trace_processor {

HeapProfileMappingTable::HeapProfileMappingTable(sqlite3*,
                                                 const TraceStorage* storage)
    : storage_(storage) {}

void HeapProfileMappingTable::RegisterTable(sqlite3* db,
                                            const TraceStorage* storage) {
  Table::Register<HeapProfileMappingTable>(db, storage, "heap_profile_mapping");
}

StorageSchema HeapProfileMappingTable::CreateStorageSchema() {
  const auto& mappings = storage_->heap_profile_mappings();
  return StorageSchema::Builder()
      .AddGenericNumericColumn("id", RowAccessor())
      .AddStringColumn("build_id", &mappings.build_ids(),
                       &storage_->string_pool())
      .AddNumericColumn("offset", &mappings.offsets())
      .AddNumericColumn("start", &mappings.starts())
      .AddNumericColumn("end", &mappings.ends())
      .AddNumericColumn("load_bias", &mappings.load_biases())
      .AddStringColumn("name", &mappings.names(), &storage_->string_pool())
      .Build({"id"});
}

uint32_t HeapProfileMappingTable::RowCount() {
  return storage_->heap_profile_mappings().size();
}

int HeapProfileMappingTable::BestIndex(const QueryConstraints&,
                                       BestIndexInfo* info) {
  info->order_by_consumed = true;
  return SQLITE_OK;
}

}  // namespace trace_processor
}  // namespace perfetto
