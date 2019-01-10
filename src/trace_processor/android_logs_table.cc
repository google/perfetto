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

#include "src/trace_processor/android_logs_table.h"

namespace perfetto {
namespace trace_processor {

AndroidLogsTable::AndroidLogsTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void AndroidLogsTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<AndroidLogsTable>(db, storage, "android_logs");
}

StorageSchema AndroidLogsTable::CreateStorageSchema() {
  const auto& alog = storage_->android_logs();
  // Note: the logs in the storage are NOT sorted by timestamp. We delegate
  // that to the on-demand sorter by calling AddNumericColumn (instead of
  // AddSortedNumericColumn).
  return StorageSchema::Builder()
      .AddNumericColumn("ts", &alog.timestamps())
      .AddNumericColumn("utid", &alog.utids())
      .AddNumericColumn("prio", &alog.prios())
      .AddStringColumn("tag", &alog.tag_ids(), &storage_->string_pool())
      .AddStringColumn("msg", &alog.msg_ids(), &storage_->string_pool())
      .Build({"ts", "utid", "msg"});
}

uint32_t AndroidLogsTable::RowCount() {
  return static_cast<uint32_t>(storage_->android_logs().size());
}

int AndroidLogsTable::BestIndex(const QueryConstraints& qc,
                                BestIndexInfo* info) {
  info->estimated_cost = static_cast<uint32_t>(storage_->android_logs().size());

  info->order_by_consumed = true;

  // Only the string columns are handled by SQLite.
  size_t tag_index = schema().ColumnIndexFromName("tag");
  size_t msg_index = schema().ColumnIndexFromName("msg");
  for (size_t i = 0; i < qc.constraints().size(); i++) {
    info->omit[i] =
        qc.constraints()[i].iColumn != static_cast<int>(tag_index) &&
        qc.constraints()[i].iColumn != static_cast<int>(msg_index);
  }

  return SQLITE_OK;
}
}  // namespace trace_processor
}  // namespace perfetto
