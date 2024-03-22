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

#include "src/trace_processor/sqlite/stats_table.h"

#include <memory>

#include "perfetto/base/status.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/sqlite/query_constraints.h"
#include "src/trace_processor/sqlite/sqlite_result.h"
#include "src/trace_processor/sqlite/sqlite_table.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor {

StatsTable::StatsTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

StatsTable::~StatsTable() = default;

base::Status StatsTable::Init(int, const char* const*, Schema* schema) {
  *schema = Schema(
      {
          SqliteTable::Column(Column::kName, "name", SqlValue::Type::kString),
          // Calling a column "index" causes sqlite to silently fail, hence idx.
          SqliteTable::Column(Column::kIndex, "idx", SqlValue::Type::kLong),
          SqliteTable::Column(Column::kSeverity, "severity",
                              SqlValue::Type::kString),
          SqliteTable::Column(Column::kSource, "source",
                              SqlValue::Type::kString),
          SqliteTable::Column(Column::kValue, "value", SqlValue::Type::kLong),
          SqliteTable::Column(Column::kDescription, "description",
                              SqlValue::Type::kString),
      },
      {Column::kName});
  return base::OkStatus();
}

std::unique_ptr<SqliteTable::BaseCursor> StatsTable::CreateCursor() {
  return std::unique_ptr<SqliteTable::BaseCursor>(new Cursor(this));
}

int StatsTable::BestIndex(const QueryConstraints&, BestIndexInfo*) {
  return SQLITE_OK;
}

StatsTable::Cursor::Cursor(StatsTable* table)
    : SqliteTable::BaseCursor(table),
      table_(table),
      storage_(table->storage_) {}

StatsTable::Cursor::~Cursor() = default;

base::Status StatsTable::Cursor::Filter(const QueryConstraints&,
                                        sqlite3_value**,
                                        FilterHistory) {
  *this = Cursor(table_);
  return base::OkStatus();
}

base::Status StatsTable::Cursor::Column(sqlite3_context* ctx, int N) {
  switch (N) {
    case Column::kName:
      sqlite::result::StaticString(ctx, stats::kNames[key_]);
      break;
    case Column::kIndex:
      if (stats::kTypes[key_] == stats::kIndexed) {
        sqlite::result::Long(ctx, index_->first);
      } else {
        sqlite::result::Null(ctx);
      }
      break;
    case Column::kSeverity:
      switch (stats::kSeverities[key_]) {
        case stats::kInfo:
          sqlite::result::StaticString(ctx, "info");
          break;
        case stats::kDataLoss:
          sqlite::result::StaticString(ctx, "data_loss");
          break;
        case stats::kError:
          sqlite::result::StaticString(ctx, "error");
          break;
      }
      break;
    case Column::kSource:
      switch (stats::kSources[key_]) {
        case stats::kTrace:
          sqlite::result::StaticString(ctx, "trace");
          break;
        case stats::kAnalysis:
          sqlite::result::StaticString(ctx, "analysis");
          break;
      }
      break;
    case Column::kValue:
      if (stats::kTypes[key_] == stats::kIndexed) {
        sqlite::result::Long(ctx, index_->second);
      } else {
        sqlite::result::Long(ctx, storage_->stats()[key_].value);
      }
      break;
    case Column::kDescription:
      sqlite::result::StaticString(ctx, stats::kDescriptions[key_]);
      break;
    default:
      PERFETTO_FATAL("Unknown column %d", N);
      break;
  }
  return base::OkStatus();
}

base::Status StatsTable::Cursor::Next() {
  static_assert(stats::kTypes[0] == stats::kSingle,
                "the first stats entry cannot be indexed");
  const auto* cur_entry = &storage_->stats()[key_];
  if (stats::kTypes[key_] == stats::kIndexed) {
    if (++index_ != cur_entry->indexed_values.end()) {
      return base::OkStatus();
    }
  }
  while (++key_ < stats::kNumKeys) {
    cur_entry = &storage_->stats()[key_];
    index_ = cur_entry->indexed_values.begin();
    if (stats::kTypes[key_] == stats::kSingle ||
        !cur_entry->indexed_values.empty()) {
      break;
    }
  }
  return base::OkStatus();
}

bool StatsTable::Cursor::Eof() {
  return key_ >= stats::kNumKeys;
}

}  // namespace perfetto::trace_processor
