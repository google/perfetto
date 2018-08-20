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

SliceTable::SliceTable(const TraceStorage* storage) : storage_(storage) {}

void SliceTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<SliceTable>(db, storage,
                              "CREATE TABLE slices("
                              "ts UNSIGNED BIG INT, "
                              "dur UNSIGNED BIG INT, "
                              "utid UNSIGNED INT,"
                              "cat STRING,"
                              "name STRING,"
                              "depth INT,"
                              "stack_id UNSIGNED BIG INT,"
                              "parent_stack_id UNSIGNED BIG INT,"
                              "PRIMARY KEY(utid, ts, depth)"
                              ") WITHOUT ROWID;");
  // TODO(primiano): add support for ts_lower_bound. It requires the guarantee
  // that slices are pushed in the storage monotonically.
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
