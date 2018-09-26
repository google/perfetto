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

#include <sqlite3.h>
#include <string.h>

#include <algorithm>
#include <bitset>
#include <numeric>

#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

SliceTable::SliceTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void SliceTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<SliceTable>(db, storage, "slices");
}

Table::Schema SliceTable::CreateSchema(int, const char* const*) {
  return Schema(
      {
          Table::Column(Column::kTimestamp, "ts", ColumnType::kUlong),
          Table::Column(Column::kDuration, "dur", ColumnType::kUlong),
          Table::Column(Column::kUtid, "utid", ColumnType::kUint),
          Table::Column(Column::kCategory, "cat", ColumnType::kString),
          Table::Column(Column::kName, "name", ColumnType::kString),
          Table::Column(Column::kDepth, "depth", ColumnType::kInt),
          Table::Column(Column::kStackId, "stack_id", ColumnType::kUlong),
          Table::Column(Column::kParentStackId, "parent_stack_id",
                        ColumnType::kUlong),
      },
      {Column::kUtid, Column::kTimestamp, Column::kDepth});
}

std::unique_ptr<Table::Cursor> SliceTable::CreateCursor() {
  return std::unique_ptr<Table::Cursor>(new Cursor(storage_));
}

int SliceTable::BestIndex(const QueryConstraints&, BestIndexInfo* info) {
  info->order_by_consumed = false;  // Delegate sorting to SQLite.
  info->estimated_cost =
      static_cast<uint32_t>(storage_->nestable_slices().slice_count());
  return SQLITE_OK;
}

SliceTable::Cursor::Cursor(const TraceStorage* storage) : storage_(storage) {
  num_rows_ = storage->nestable_slices().slice_count();
}

SliceTable::Cursor::~Cursor() = default;

int SliceTable::Cursor::Filter(const QueryConstraints&,
                               sqlite3_value** /*argv*/) {
  return SQLITE_OK;
}

int SliceTable::Cursor::Next() {
  row_++;
  return SQLITE_OK;
}

int SliceTable::Cursor::Eof() {
  return row_ >= num_rows_;
}

int SliceTable::Cursor::Column(sqlite3_context* context, int col) {
  const auto& slices = storage_->nestable_slices();
  switch (col) {
    case Column::kTimestamp:
      sqlite3_result_int64(context,
                           static_cast<sqlite3_int64>(slices.start_ns()[row_]));
      break;
    case Column::kDuration:
      sqlite3_result_int64(
          context, static_cast<sqlite3_int64>(slices.durations()[row_]));
      break;
    case Column::kUtid:
      sqlite3_result_int64(context,
                           static_cast<sqlite3_int64>(slices.utids()[row_]));
      break;
    case Column::kCategory:
      sqlite3_result_text(context,
                          storage_->GetString(slices.cats()[row_]).c_str(), -1,
                          nullptr);
      break;
    case Column::kName:
      sqlite3_result_text(context,
                          storage_->GetString(slices.names()[row_]).c_str(), -1,
                          nullptr);
      break;
    case Column::kDepth:
      sqlite3_result_int64(context,
                           static_cast<sqlite3_int64>(slices.depths()[row_]));
      break;
    case Column::kStackId:
      sqlite3_result_int64(
          context, static_cast<sqlite3_int64>(slices.stack_ids()[row_]));
      break;
    case Column::kParentStackId:
      sqlite3_result_int64(
          context, static_cast<sqlite3_int64>(slices.parent_stack_ids()[row_]));
      break;
  }
  return SQLITE_OK;
}

}  // namespace trace_processor
}  // namespace perfetto
