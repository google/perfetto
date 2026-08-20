/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/exec/sql_scan.h"

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"

namespace perfetto::trace_processor::exec {
namespace {

using core::Double;
using core::Int64;
using core::StorageType;
using core::String;
using core::exec::ColumnView;
using core::exec::kMaxBatchRows;
using core::exec::RowBatch;
using core::exec::RowSelection;

const char* TypeName(StorageType type) {
  if (type.Is<Int64>()) {
    return "an integer";
  }
  if (type.Is<Double>()) {
    return "a float";
  }
  return "a string";
}

}  // namespace

base::StatusOr<std::unique_ptr<SqlScan>>
SqlScan::Create(SqliteConnection* connection, SqlSource sql, StringPool* pool) {
  SqliteConnection::PreparedStatement statement =
      connection->PrepareStatement(std::move(sql));
  RETURN_IF_ERROR(statement.status());

  // The columns are known once the statement is prepared, before any row is
  // read, which is what lets a stage resolve the names it was given first.
  sqlite3_stmt* stmt = statement.sqlite_stmt();
  auto count = static_cast<uint32_t>(sqlite3_column_count(stmt));
  std::vector<std::string> names;
  names.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const char* name = sqlite3_column_name(stmt, static_cast<int>(i));
    names.emplace_back(name ? name : "");
  }
  return std::unique_ptr<SqlScan>(
      new SqlScan(std::move(statement), std::move(names), pool));
}

SqlScan::SqlScan(SqliteConnection::PreparedStatement statement,
                 std::vector<std::string> names,
                 StringPool* pool)
    : statement_(std::move(statement)),
      names_(std::move(names)),
      pool_(pool),
      columns_(names_.size()) {
  for (Column& column : columns_) {
    column.validity = core::BitVector::CreateWithSize(kMaxBatchRows);
  }
}

SqlScan::~SqlScan() = default;

void SqlScan::Reset() {
  sqlite3_reset(statement_.sqlite_stmt());
  done_ = false;
}

bool SqlScan::Settle(uint32_t index, StorageType type) {
  Column& column = columns_[index];
  if (column.type) {
    if (*column.type == type) {
      return true;
    }
    status_ = base::ErrStatus(
        "SQL source: column '%s' holds %s after already holding %s; a "
        "column's type is settled by its first value and cannot change",
        names_[index].c_str(), TypeName(type), TypeName(*column.type));
    return false;
  }
  column.type = type;
  if (type.Is<Int64>()) {
    column.ints.resize(kMaxBatchRows);
  } else if (type.Is<Double>()) {
    column.doubles.resize(kMaxBatchRows);
  } else {
    column.strings.resize(kMaxBatchRows);
  }
  return true;
}

bool SqlScan::ReadValue(sqlite3_stmt* stmt, uint32_t index, uint32_t row) {
  Column& column = columns_[index];
  auto col = static_cast<int>(index);
  switch (sqlite3_column_type(stmt, col)) {
    case SQLITE_NULL:
      // The validity bit was cleared when the batch started.
      return true;
    case SQLITE_INTEGER:
      if (!Settle(index, StorageType{Int64{}})) {
        return false;
      }
      column.ints[row] = sqlite3_column_int64(stmt, col);
      break;
    case SQLITE_FLOAT:
      if (!Settle(index, StorageType{Double{}})) {
        return false;
      }
      column.doubles[row] = sqlite3_column_double(stmt, col);
      break;
    case SQLITE_TEXT:
      if (!Settle(index, StorageType{String{}})) {
        return false;
      }
      column.strings[row] = pool_->InternString(
          reinterpret_cast<const char*>(sqlite3_column_text(stmt, col)));
      break;
    default:
      status_ = base::ErrStatus(
          "SQL source: column '%s' holds a blob, which a pipeline cannot "
          "carry",
          names_[index].c_str());
      return false;
  }
  column.validity.set(row);
  return true;
}

ColumnView SqlScan::MakeView(const Column& column) const {
  const void* data = nullptr;
  if (column.type->Is<Int64>()) {
    data = column.ints.data();
  } else if (column.type->Is<Double>()) {
    data = column.doubles.data();
  } else {
    data = column.strings.data();
  }
  return ColumnView::Reference(*column.type, data, &column.validity);
}

RowBatch* SqlScan::Next() {
  if (done_ || !status_.ok()) {
    return nullptr;
  }
  for (Column& column : columns_) {
    column.validity.ClearAllBits();
  }

  sqlite3_stmt* stmt = statement_.sqlite_stmt();
  uint32_t count = 0;
  while (count < kMaxBatchRows && statement_.Step()) {
    for (uint32_t i = 0; i < columns_.size(); ++i) {
      if (!ReadValue(stmt, i, count)) {
        return nullptr;
      }
    }
    ++count;
  }
  if (!statement_.status().ok()) {
    status_ = statement_.status();
    return nullptr;
  }
  done_ = count < kMaxBatchRows;
  if (count == 0) {
    return nullptr;
  }

  if (batch_.column_count() == 0) {
    for (uint32_t i = 0; i < columns_.size(); ++i) {
      // A column which never held a value still has to be some type for
      // anything to read it; nothing will, because none of its rows are valid.
      if (!columns_[i].type) {
        Settle(i, StorageType{Int64{}});
      }
      pristine_.push_back(MakeView(columns_[i]));
      batch_.AddColumn(pristine_.back());
    }
  }

  batch_.PrepareForFill();
  for (uint32_t i = 0; i < columns_.size(); ++i) {
    // Operators only replace a column's row view, so restoring the views is
    // all a reused batch needs before being filled again.
    batch_.mutable_column(i).AdoptSelection(pristine_[i]);
  }
  batch_.Compose(RowSelection::Range(0), count);
  batch_.SetCardinality(count);
  return &batch_;
}

}  // namespace perfetto::trace_processor::exec
