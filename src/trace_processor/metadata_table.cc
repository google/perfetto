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

#include "src/trace_processor/metadata_table.h"
#include "src/trace_processor/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

MetadataTable::MetadataTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void MetadataTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<MetadataTable>(db, storage, "metadata");
}

util::Status MetadataTable::Init(int, const char* const*, Schema* schema) {
  *schema = Schema(
      {
          Table::Column(Column::kName, "name", ColumnType::kString),
          Table::Column(Column::kKeyType, "key_type", ColumnType::kString),
          Table::Column(Column::kIntValue, "int_value", ColumnType::kLong),
          Table::Column(Column::kStrValue, "str_value", ColumnType::kString),
      },
      {Column::kName});
  return util::OkStatus();
}

std::unique_ptr<Table::Cursor> MetadataTable::CreateCursor() {
  return std::unique_ptr<Table::Cursor>(new Cursor(this));
}

int MetadataTable::BestIndex(const QueryConstraints&, BestIndexInfo*) {
  return SQLITE_OK;
}

MetadataTable::Cursor::Cursor(MetadataTable* table)
    : Table::Cursor(table), table_(table), storage_(table->storage_) {
  for (key_ = 0; key_ < metadata::kNumKeys; ++key_) {
    const auto* cur_entry = &storage_->metadata()[key_];
    if (!cur_entry->empty()) {
      iter_ = cur_entry->begin();
      break;
    }
  }
}

int MetadataTable::Cursor::Filter(const QueryConstraints&, sqlite3_value**) {
  *this = Cursor(table_);
  return SQLITE_OK;
}

int MetadataTable::Cursor::Column(sqlite3_context* ctx, int N) {
  const auto kSqliteStatic = sqlite_utils::kSqliteStatic;
  switch (N) {
    case Column::kName:
      sqlite3_result_text(ctx, metadata::kNames[key_], -1, kSqliteStatic);
      break;
    case Column::kKeyType:
      switch (metadata::kKeyTypes[key_]) {
        case metadata::kSingle:
          sqlite3_result_text(ctx, "single", -1, kSqliteStatic);
          break;
        case metadata::kMulti:
          sqlite3_result_text(ctx, "multi", -1, kSqliteStatic);
          break;
      }
      break;
    case Column::kIntValue:
      if (metadata::kValueTypes[key_] != Variadic::kInt) {
        sqlite3_result_null(ctx);
        break;
      }
      sqlite3_result_int64(ctx, iter_->int_value);
      break;
    case Column::kStrValue:
      if (metadata::kValueTypes[key_] != Variadic::kString) {
        sqlite3_result_null(ctx);
        break;
      }
      sqlite3_result_text(ctx, storage_->GetString(iter_->string_value).c_str(),
                          -1, kSqliteStatic);
      break;
    default:
      PERFETTO_FATAL("Unknown column %d", N);
      break;
  }
  return SQLITE_OK;
}

int MetadataTable::Cursor::Next() {
  const auto* cur_entry = &storage_->metadata()[key_];
  if (++iter_ != cur_entry->end()) {
    return SQLITE_OK;
  }
  while (++key_ < metadata::kNumKeys) {
    cur_entry = &storage_->metadata()[key_];
    if (!cur_entry->empty()) {
      iter_ = cur_entry->begin();
      break;
    }
  }
  return SQLITE_OK;
}

int MetadataTable::Cursor::Eof() {
  return key_ >= metadata::kNumKeys;
}

}  // namespace trace_processor
}  // namespace perfetto
