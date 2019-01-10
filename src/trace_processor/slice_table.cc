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

#include "src/trace_processor/slice_table.h"

#include "src/trace_processor/storage_columns.h"

namespace perfetto {
namespace trace_processor {

SliceTable::SliceTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void SliceTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<SliceTable>(db, storage, "slices");
}

StorageSchema SliceTable::CreateStorageSchema() {
  const auto& slices = storage_->nestable_slices();
  return StorageSchema::Builder()
      .AddOrderedNumericColumn("ts", &slices.start_ns())
      .AddNumericColumn("dur", &slices.durations())
      .AddNumericColumn("utid", &slices.utids())
      .AddStringColumn("cat", &slices.cats(), &storage_->string_pool())
      .AddStringColumn("name", &slices.names(), &storage_->string_pool())
      .AddNumericColumn("depth", &slices.depths())
      .AddNumericColumn("stack_id", &slices.stack_ids())
      .AddNumericColumn("parent_stack_id", &slices.parent_stack_ids())
      .Build({"utid", "ts", "depth"});
}

uint32_t SliceTable::RowCount() {
  return static_cast<uint32_t>(storage_->nestable_slices().slice_count());
}

int SliceTable::BestIndex(const QueryConstraints& qc, BestIndexInfo* info) {
  info->estimated_cost =
      static_cast<uint32_t>(storage_->nestable_slices().slice_count());

  // Only the string columns are handled by SQLite
  info->order_by_consumed = true;
  size_t name_index = schema().ColumnIndexFromName("name");
  size_t cat_index = schema().ColumnIndexFromName("cat");
  for (size_t i = 0; i < qc.constraints().size(); i++) {
    info->omit[i] =
        qc.constraints()[i].iColumn != static_cast<int>(name_index) &&
        qc.constraints()[i].iColumn != static_cast<int>(cat_index);
  }
  return SQLITE_OK;
}

}  // namespace trace_processor
}  // namespace perfetto
