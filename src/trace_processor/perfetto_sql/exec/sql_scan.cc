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
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/perfetto_sql/lineage/column_lineage.h"
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
using core::exec::Variant;

// The types lineage established, lined up with the query's columns. If the two
// disagree on the number of columns they are not describing the same query, so
// no type is claimed for any of them.
std::vector<std::optional<StorageType>> Prove(const SqlSource& sql,
                                              uint32_t count,
                                              const lineage::Catalog* catalog) {
  std::vector<std::optional<StorageType>> types(count);
  if (!catalog) {
    return types;
  }
  auto resolved = lineage::ResolveSelect(sql.sql(), *catalog);
  if (!resolved.ok() || resolved->size() != count) {
    return types;
  }
  for (uint32_t i = 0; i < count; ++i) {
    std::optional<StorageType> type = (*resolved)[i].type;
    // An Id has no storage of its own: its value is the row it sits at. A
    // query result has no such rows to point at, so materialise it at the
    // narrowest width which holds one.
    if (type && type->Is<core::Id>()) {
      type = StorageType{core::Uint32{}};
    }
    types[i] = type;
  }
  return types;
}

}  // namespace

base::StatusOr<std::unique_ptr<SqlScan>> SqlScan::Create(
    SqliteConnection* connection,
    SqlSource sql,
    StringPool* pool,
    const lineage::Catalog* catalog) {
  // Prepared here only to read the column names, then discarded: a statement
  // belongs to one execution, but the columns belong to the query.
  SqliteConnection::PreparedStatement statement =
      connection->PrepareStatement(sql);
  RETURN_IF_ERROR(statement.status());

  sqlite3_stmt* stmt = statement.sqlite_stmt();
  auto count = static_cast<uint32_t>(sqlite3_column_count(stmt));
  std::vector<std::string> names;
  names.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const char* name = sqlite3_column_name(stmt, static_cast<int>(i));
    names.emplace_back(name ? name : "");
  }
  std::vector<std::optional<StorageType>> types = Prove(sql, count, catalog);
  return std::unique_ptr<SqlScan>(new SqlScan(
      connection, std::move(sql), std::move(names), std::move(types), pool));
}

SqlScan::SqlScan(SqliteConnection* connection,
                 SqlSource sql,
                 std::vector<std::string> names,
                 std::vector<std::optional<StorageType>> types,
                 StringPool* pool)
    : connection_(connection),
      sql_(std::move(sql)),
      names_(std::move(names)),
      types_(std::move(types)),
      pool_(pool) {}

SqlScan::~SqlScan() = default;
SqlScan::State::~State() = default;

std::unique_ptr<core::exec::OperatorState> SqlScan::MakeState() const {
  auto state = std::make_unique<State>();
  state->statement.emplace(connection_->PrepareStatement(sql_));
  state->status = state->statement->status();
  state->columns.reserve(names_.size());
  for (uint32_t i = 0; i < names_.size(); ++i) {
    auto column = std::make_shared<Column>();
    if (!types_[i]) {
      column->variants.resize(kMaxBatchRows);
    } else if (types_[i]->Is<core::Uint32>()) {
      column->uint32s.resize(kMaxBatchRows);
    } else if (types_[i]->Is<core::Int32>()) {
      column->int32s.resize(kMaxBatchRows);
    } else if (types_[i]->Is<Int64>()) {
      column->ints.resize(kMaxBatchRows);
    } else if (types_[i]->Is<Double>()) {
      column->doubles.resize(kMaxBatchRows);
    } else {
      column->strings.resize(kMaxBatchRows);
    }
    column->validity = core::BitVector::CreateWithSize(kMaxBatchRows);
    state->columns.push_back(std::move(column));
  }
  return state;
}

base::Status SqlScan::status(const core::exec::OperatorState& state) const {
  return state.Cast<const State>().status;
}

void SqlScan::Rewind(core::exec::OperatorState& state) const {
  State& s = state.Cast<State>();
  sqlite3_reset(s.statement->sqlite_stmt());
  s.done = false;
}

bool SqlScan::ReadValue(State& s,
                        sqlite3_stmt* stmt,
                        uint32_t index,
                        uint32_t row) const {
  auto col = static_cast<int>(index);
  Column& column = *s.columns[index];
  int type = sqlite3_column_type(stmt, col);
  if (!types_[index]) {
    switch (type) {
      case SQLITE_INTEGER:
        column.variants[row] = Variant::Int64(sqlite3_column_int64(stmt, col));
        return true;
      case SQLITE_FLOAT:
        column.variants[row] =
            Variant::Double(sqlite3_column_double(stmt, col));
        return true;
      case SQLITE_TEXT:
        column.variants[row] = Variant::String(pool_->InternString(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, col))));
        return true;
      case SQLITE_NULL:
        column.variants[row] = Variant::Null();
        return true;
      default:
        s.status = base::ErrStatus(
            "SQL source: column '%s' holds a blob, which a pipeline cannot "
            "carry",
            names_[index].c_str());
        return false;
    }
  }
  StorageType proven = *types_[index];
  if (type == SQLITE_NULL) {
    // The row is null, but write the slot anyway. A flat column's storage is
    // readable at every row, so a reader summing it needs no per-row branch and
    // never sees a value left over from the previous batch.
    if (proven.Is<core::Uint32>()) {
      column.uint32s[row] = 0;
    } else if (proven.Is<core::Int32>()) {
      column.int32s[row] = 0;
    } else if (proven.Is<Int64>()) {
      column.ints[row] = 0;
    } else if (proven.Is<Double>()) {
      column.doubles[row] = 0;
    } else {
      column.strings[row] = StringPool::Id::Null();
    }
    return true;
  }
  bool integer = proven.Is<Int64>() || proven.Is<core::Uint32>() ||
                 proven.Is<core::Int32>();
  bool matches = (type == SQLITE_INTEGER && integer) ||
                 (type == SQLITE_FLOAT && proven.Is<Double>()) ||
                 (type == SQLITE_TEXT && proven.Is<String>());
  if (!matches) {
    // Only reachable if the type lineage established turned out to be wrong.
    s.status = base::ErrStatus(
        "SQL source: column '%s' does not hold what it was traced back to",
        names_[index].c_str());
    return false;
  }
  if (proven.Is<core::Uint32>()) {
    column.uint32s[row] =
        static_cast<uint32_t>(sqlite3_column_int64(stmt, col));
  } else if (proven.Is<core::Int32>()) {
    column.int32s[row] = static_cast<int32_t>(sqlite3_column_int64(stmt, col));
  } else if (proven.Is<Int64>()) {
    column.ints[row] = sqlite3_column_int64(stmt, col);
  } else if (proven.Is<Double>()) {
    column.doubles[row] = sqlite3_column_double(stmt, col);
  } else {
    column.strings[row] = pool_->InternString(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, col)));
  }
  column.validity.set(row);
  return true;
}

bool SqlScan::GetData(RowBatch& out, core::exec::OperatorState& state) const {
  State& s = state.Cast<State>();
  if (s.done || !s.status.ok()) {
    return false;
  }
  for (const std::shared_ptr<Column>& column : s.columns) {
    column->validity.ClearAllBits();
  }
  sqlite3_stmt* stmt = s.statement->sqlite_stmt();
  uint32_t count = 0;
  while (count < kMaxBatchRows && s.statement->Step()) {
    for (uint32_t i = 0; i < s.columns.size(); ++i) {
      if (!ReadValue(s, stmt, i, count)) {
        return false;
      }
    }
    ++count;
  }
  if (!s.statement->status().ok()) {
    s.status = s.statement->status();
    return false;
  }
  s.done = count < kMaxBatchRows;
  if (count == 0) {
    return false;
  }

  out.Reset();
  for (uint32_t i = 0; i < s.columns.size(); ++i) {
    const std::shared_ptr<Column>& column = s.columns[i];
    if (!types_[i]) {
      out.AddColumn(ColumnView::Variants(column->variants.data()), column);
      continue;
    }
    const void* data = nullptr;
    if (types_[i]->Is<core::Uint32>()) {
      data = column->uint32s.data();
    } else if (types_[i]->Is<core::Int32>()) {
      data = column->int32s.data();
    } else if (types_[i]->Is<Int64>()) {
      data = column->ints.data();
    } else if (types_[i]->Is<Double>()) {
      data = column->doubles.data();
    } else {
      data = column->strings.data();
    }
    out.AddColumn(ColumnView::Reference(*types_[i], data, &column->validity),
                  column);
  }
  out.Compose(RowSelection::Range(0), count);
  out.SetCardinality(count);
  return true;
}

}  // namespace perfetto::trace_processor::exec
