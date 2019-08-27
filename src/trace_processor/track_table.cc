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

#include "src/trace_processor/track_table.h"

namespace perfetto {
namespace trace_processor {

TrackTable::TrackTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void TrackTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  SqliteTable::Register<TrackTable>(db, storage, "track");
}

StorageSchema TrackTable::CreateStorageSchema() {
  const auto& tracks = storage_->tracks();
  return StorageSchema::Builder()
      .AddGenericNumericColumn("id", RowAccessor())
      .AddStringColumn("name", &tracks.names(), &storage_->string_pool())
      .Build({"id"});
}

uint32_t TrackTable::RowCount() {
  return storage_->tracks().track_count();
}

int TrackTable::BestIndex(const QueryConstraints& qc, BestIndexInfo* info) {
  info->order_by_consumed = true;
  info->estimated_cost = HasEqConstraint(qc, "id") ? 1 : RowCount();
  return SQLITE_OK;
}

}  // namespace trace_processor
}  // namespace perfetto
