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

#include "src/trace_processor/heap_profile_allocation_table.h"

namespace perfetto {
namespace trace_processor {

HeapProfileAllocationTable::HeapProfileAllocationTable(
    sqlite3*,
    const TraceStorage* storage)
    : storage_(storage) {}

void HeapProfileAllocationTable::RegisterTable(sqlite3* db,
                                               const TraceStorage* storage) {
  Table::Register<HeapProfileAllocationTable>(db, storage,
                                              "heap_profile_allocation");
}

StorageSchema HeapProfileAllocationTable::CreateStorageSchema() {
  const auto& allocs = storage_->heap_profile_allocations();
  return StorageSchema::Builder()
      .AddGenericNumericColumn("id", RowAccessor())
      .AddOrderedNumericColumn("ts", &allocs.timestamps())
      .AddNumericColumn("pid", &allocs.pids())
      .AddNumericColumn("callsite_id", &allocs.callsite_ids())
      .AddNumericColumn("count", &allocs.counts())
      .AddNumericColumn("size", &allocs.sizes())
      .Build({"id"});
}

uint32_t HeapProfileAllocationTable::RowCount() {
  return storage_->heap_profile_allocations().size();
}

int HeapProfileAllocationTable::BestIndex(const QueryConstraints&,
                                          BestIndexInfo* info) {
  info->order_by_consumed = true;
  return SQLITE_OK;
}

}  // namespace trace_processor
}  // namespace perfetto
