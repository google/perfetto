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

#include "src/trace_processor/string_table.h"

#include <sqlite3.h>
#include <string.h>

#include <algorithm>
#include <bitset>
#include <numeric>

#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

StringTable::StringTable(const TraceStorage* storage) : storage_(storage) {}

void StringTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<StringTable>(db, storage,
                               "CREATE TABLE strings("
                               "id UNSIGNED BIG INT, "
                               "str STRING,"
                               "PRIMARY KEY(id)"
                               ") WITHOUT ROWID;");
}

std::unique_ptr<Table::Cursor> StringTable::CreateCursor() {
  return std::unique_ptr<Table::Cursor>(new Cursor(storage_));
}

int StringTable::BestIndex(const QueryConstraints&, BestIndexInfo* info) {
  info->order_by_consumed = false;  // Delegate sorting to SQLite.
  info->estimated_cost = static_cast<uint32_t>(storage_->string_count());
  return SQLITE_OK;
}

StringTable::Cursor::Cursor(const TraceStorage* storage) : storage_(storage) {
  num_rows_ = storage->string_count();
}

StringTable::Cursor::~Cursor() = default;

int StringTable::Cursor::Filter(const QueryConstraints&,
                                sqlite3_value** /*argv*/) {
  return SQLITE_OK;
}

int StringTable::Cursor::Next() {
  row_++;
  return SQLITE_OK;
}

int StringTable::Cursor::Eof() {
  return row_ >= num_rows_;
}

int StringTable::Cursor::Column(sqlite3_context* context, int col) {
  StringId string_id = static_cast<StringId>(row_);
  switch (col) {
    case Column::kStringId:
      sqlite3_result_int64(context, static_cast<sqlite3_int64>(row_));
      break;
    case Column::kString:
      sqlite3_result_text(context, storage_->GetString(string_id).c_str(), -1,
                          nullptr);
      break;
  }
  return SQLITE_OK;
}

}  // namespace trace_processor
}  // namespace perfetto
