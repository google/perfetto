/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/sqlite_experimental_counter_dur_table.h"

#include "src/trace_processor/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

SqliteExperimentalCounterDurTable::SqliteExperimentalCounterDurTable(
    sqlite3*,
    Context context)
    : cache_(context.cache), counter_table_(context.table) {}

SqliteExperimentalCounterDurTable::~SqliteExperimentalCounterDurTable() =
    default;

void SqliteExperimentalCounterDurTable::RegisterTable(
    sqlite3* db,
    QueryCache* cache,
    const tables::CounterTable& table) {
  SqliteTable::Register<SqliteExperimentalCounterDurTable>(
      db, Context{cache, &table}, "experimental_counter_dur");
}

util::Status SqliteExperimentalCounterDurTable::Init(
    int,
    const char* const*,
    SqliteTable::Schema* schema) {
  *schema = DbSqliteTable::ComputeSchema(*counter_table_, name().c_str());
  schema->mutable_columns()->emplace_back(
      Column(schema->columns().size(), "dur", SqlValue::Type::kLong));
  return util::OkStatus();
}

int SqliteExperimentalCounterDurTable::BestIndex(const QueryConstraints& qc,
                                                 BestIndexInfo* info) {
  // Add the outline for the dur column.
  auto outline = DbSqliteTable::OutlineFromTable(*counter_table_);
  outline.columns.push_back({false /* is_id */, false /* is_sorted */});

  DbSqliteTable::BestIndex(outline, qc, info);
  return SQLITE_OK;
}

std::unique_ptr<SqliteTable::Cursor>
SqliteExperimentalCounterDurTable::CreateCursor() {
  std::unique_ptr<TableAndColumn> table_and_column(new TableAndColumn());
  table_and_column->dur = ComputeDurColumn(*counter_table_);
  table_and_column->table = counter_table_->ExtendWithColumn(
      "dur", &table_and_column->dur, TypedColumn<int64_t>::default_flags());
  return std::unique_ptr<Cursor>(
      new Cursor(this, cache_, std::move(table_and_column)));
}

// static
SparseVector<int64_t> SqliteExperimentalCounterDurTable::ComputeDurColumn(
    const tables::CounterTable& table) {
  // Keep track of the last seen row for each track id.
  std::unordered_map<TrackId, uint32_t> last_row_for_track_id;
  SparseVector<int64_t> dur;
  for (uint32_t i = 0; i < table.row_count(); ++i) {
    // Check if we already have a previous row for the current track id.
    TrackId track_id = table.track_id()[i];
    auto it = last_row_for_track_id.find(track_id);
    if (it == last_row_for_track_id.end()) {
      // This means we don't have any row - start tracking this row for the
      // future.
      last_row_for_track_id.emplace(track_id, i);
    } else {
      // This means we have an previous row for the current track id. Update
      // the duration of the previous row to be up to the current ts.
      uint32_t old_row = it->second;
      it->second = i;
      dur.Set(old_row, table.ts()[i] - table.ts()[old_row]);
    }
    // Append -1 to make this event as not having been finished. On a later
    // row, we may set this to have the correct value.
    dur.Append(-1);
  }
  return dur;
}

SqliteExperimentalCounterDurTable::Cursor::Cursor(
    SqliteTable* sqlite_table,
    QueryCache* cache,
    std::unique_ptr<TableAndColumn> table_and_column)
    : DbSqliteTable::Cursor(sqlite_table, cache, &table_and_column->table),
      table_and_column_(std::move(table_and_column)) {}

SqliteExperimentalCounterDurTable::Cursor::~Cursor() = default;

}  // namespace trace_processor
}  // namespace perfetto
