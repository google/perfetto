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

#include "src/trace_processor/heap_profile_frame_table.h"

namespace perfetto {
namespace trace_processor {

HeapProfileFrameTable::HeapProfileFrameTable(sqlite3*,
                                             const TraceStorage* storage)
    : storage_(storage) {}

void HeapProfileFrameTable::RegisterTable(sqlite3* db,
                                          const TraceStorage* storage) {
  Table::Register<HeapProfileFrameTable>(db, storage, "heap_profile_frame");
}

StorageSchema HeapProfileFrameTable::CreateStorageSchema() {
  const auto& frames = storage_->heap_profile_frames();
  return StorageSchema::Builder()
      .AddGenericNumericColumn("id", RowAccessor())
      .AddStringColumn("name", &frames.names(), &storage_->string_pool())
      .AddNumericColumn("mapping", &frames.mappings())
      .AddNumericColumn("rel_pc", &frames.rel_pcs())
      .Build({"id"});
}

uint32_t HeapProfileFrameTable::RowCount() {
  return storage_->heap_profile_frames().size();
}

int HeapProfileFrameTable::BestIndex(const QueryConstraints&,
                                     BestIndexInfo* info) {
  info->order_by_consumed = true;
  return SQLITE_OK;
}

}  // namespace trace_processor
}  // namespace perfetto
