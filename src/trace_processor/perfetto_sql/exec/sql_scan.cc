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
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/perfetto_sql/lineage/type_mapping.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"

namespace perfetto::trace_processor::exec {
namespace {

namespace analysis = ::perfetto::perfetto_sql::analysis;

using core::Double;
using core::Int64;
using core::StorageType;
using core::String;
using core::exec::ColumnChunk;
using core::exec::ColumnView;
using core::exec::kMaxBatchRows;
using core::exec::RowBatch;
using core::exec::RowSelection;
using core::exec::Variant;
struct ParserDeleter {
  void operator()(SyntaqliteParser* parser) const {
    syntaqlite_parser_destroy(parser);
  }
};
using ScopedParser = std::unique_ptr<SyntaqliteParser, ParserDeleter>;

// The types lineage established, lined up with the query's columns. If the two
// disagree on the number of columns they are not describing the same query, so
// no type is claimed for any of them.
std::vector<std::optional<StorageType>> ResolveTypes(
    const SqlSource& sql,
    uint32_t count,
    const analysis::Catalog& catalog) {
  std::vector<std::optional<StorageType>> types(count);
  ScopedParser parser(syntaqlite_parser_create_perfetto(nullptr));
  syntaqlite_parser_reset(parser.get(), sql.sql().data(),
                          static_cast<uint32_t>(sql.sql().size()));
  if (syntaqlite_parser_next(parser.get()) != SYNTAQLITE_PARSE_OK) {
    return types;
  }
  analysis::RelationAnalyzer analyzer(catalog);
  auto resolved = analyzer.AnalyzeQuery(
      {parser.get(), syntaqlite_result_root(parser.get())});
  if (!resolved.ok() || resolved->columns().size() != count) {
    return types;
  }
  for (uint32_t i = 0; i < count; ++i) {
    std::optional<analysis::ColumnType> type = resolved->columns()[i].type();
    if (!type) {
      continue;
    }
    StorageType storage = lineage::ToStorageType(*type);
    // An Id has no storage of its own: its value is the row it sits at. A
    // query result has no such rows to point at, so materialise it at the
    // narrowest width which holds one.
    if (storage.Is<core::Id>()) {
      storage = StorageType{core::Uint32{}};
    }
    types[i] = storage;
  }
  return types;
}

}  // namespace

base::StatusOr<std::unique_ptr<SqlScan>> SqlScan::Create(
    SqliteConnection* connection,
    SqlSource sql,
    StringPool* pool,
    const analysis::Catalog& catalog) {
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
  std::vector<std::optional<StorageType>> types =
      ResolveTypes(sql, count, catalog);
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
  Prepare(*state);
  state->columns.reserve(names_.size());
  state->data.reserve(names_.size());
  for (uint32_t i = 0; i < names_.size(); ++i) {
    auto column = std::make_shared<ColumnChunk>();
    void* data = nullptr;
    if (!types_[i]) {
      data = column->Values<Variant>().data();
    } else {
      switch (types_[i]->index()) {
        case StorageType::GetTypeIndex<core::Uint32>():
          data = column->Values<uint32_t>().data();
          break;
        case StorageType::GetTypeIndex<core::Int32>():
          data = column->Values<int32_t>().data();
          break;
        case StorageType::GetTypeIndex<Int64>():
          data = column->Values<int64_t>().data();
          break;
        case StorageType::GetTypeIndex<Double>():
          data = column->Values<double>().data();
          break;
        case StorageType::GetTypeIndex<String>():
          data = column->Values<StringPool::Id>().data();
          break;
        default:
          // An Id was already materialised as a Uint32 by ResolveTypes.
          PERFETTO_FATAL("Unreachable");
      }
      column->validity = core::BitVector::CreateWithSize(kMaxBatchRows);
    }
    state->columns.push_back(std::move(column));
    state->data.push_back(data);
  }
  return state;
}

void SqlScan::Prepare(State& state) const {
  state.statement.emplace(connection_->PrepareStatement(sql_));
  state.status = state.statement->status();
  state.done = false;
  if (!state.status.ok()) {
    return;
  }
  sqlite3_stmt* stmt = state.statement->sqlite_stmt();
  uint32_t count = static_cast<uint32_t>(sqlite3_column_count(stmt));
  if (count != names_.size()) {
    state.status =
        base::ErrStatus("SQL source: result shape changed between executions");
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const char* name = sqlite3_column_name(stmt, static_cast<int>(i));
    if (names_[i] != (name ? name : "")) {
      state.status = base::ErrStatus(
          "SQL source: result shape changed between executions");
      return;
    }
  }
}

base::Status SqlScan::status(const core::exec::OperatorState& state) const {
  return state.Cast<const State>().status;
}

void SqlScan::Rewind(core::exec::OperatorState& state) const {
  Prepare(state.Cast<State>());
}

bool SqlScan::ReadValue(State& s,
                        sqlite3_stmt* stmt,
                        uint32_t index,
                        uint32_t row) const {
  auto col = static_cast<int>(index);
  if (!types_[index]) {
    auto* data = static_cast<Variant*>(s.data[index]);
    switch (sqlite3_column_type(stmt, col)) {
      case SQLITE_INTEGER:
        data[row] = Variant::Int64(sqlite3_column_int64(stmt, col));
        return true;
      case SQLITE_FLOAT:
        data[row] = Variant::Double(sqlite3_column_double(stmt, col));
        return true;
      case SQLITE_TEXT:
        data[row] = Variant::String(pool_->InternString(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, col))));
        return true;
      case SQLITE_NULL:
        data[row] = Variant::Null();
        return true;
      default:
        s.status = base::ErrStatus(
            "SQL source: column '%s' holds a blob, which a pipeline cannot "
            "carry",
            names_[index].c_str());
        return false;
    }
  }
  switch (types_[index]->index()) {
    case StorageType::GetTypeIndex<core::Uint32>():
      return ReadTypedValue<uint32_t, SQLITE_INTEGER>(s, stmt, index, row);
    case StorageType::GetTypeIndex<core::Int32>():
      return ReadTypedValue<int32_t, SQLITE_INTEGER>(s, stmt, index, row);
    case StorageType::GetTypeIndex<Int64>():
      return ReadTypedValue<int64_t, SQLITE_INTEGER>(s, stmt, index, row);
    case StorageType::GetTypeIndex<Double>():
      return ReadTypedValue<double, SQLITE_FLOAT>(s, stmt, index, row);
    case StorageType::GetTypeIndex<String>():
      return ReadTypedValue<StringPool::Id, SQLITE_TEXT>(s, stmt, index, row);
    default:
      // An Id was already materialised as a Uint32 by ResolveTypes.
      PERFETTO_FATAL("Unreachable");
  }
}

template <typename T, int SqliteType>
bool SqlScan::ReadTypedValue(State& s,
                             sqlite3_stmt* stmt,
                             uint32_t index,
                             uint32_t row) const {
  auto col = static_cast<int>(index);
  auto* data = static_cast<T*>(s.data[index]);
  int type = sqlite3_column_type(stmt, col);
  if (PERFETTO_LIKELY(type == SqliteType)) {
    if constexpr (std::is_same_v<T, StringPool::Id>) {
      data[row] = pool_->InternString(
          reinterpret_cast<const char*>(sqlite3_column_text(stmt, col)));
    } else if constexpr (std::is_same_v<T, double>) {
      data[row] = sqlite3_column_double(stmt, col);
    } else {
      data[row] = static_cast<T>(sqlite3_column_int64(stmt, col));
    }
    s.columns[index]->validity.set(row);
    return true;
  }
  if (type == SQLITE_NULL) {
    // The row is null, but write the slot anyway. A flat column's storage is
    // readable at every row, so a reader summing it needs no per-row branch and
    // never sees a value left over from the previous batch.
    if constexpr (std::is_same_v<T, StringPool::Id>) {
      data[row] = StringPool::Id::Null();
    } else {
      data[row] = T{};
    }
    return true;
  }
  // Only reachable if the type lineage established turned out to be wrong.
  s.status = base::ErrStatus(
      "SQL source: column '%s' does not hold what it was traced back to",
      names_[index].c_str());
  return false;
}

bool SqlScan::GetData(RowBatch& out, core::exec::OperatorState& state) const {
  State& s = state.Cast<State>();
  if (s.done || !s.status.ok()) {
    return false;
  }
  for (const std::shared_ptr<ColumnChunk>& column : s.columns) {
    if (column->validity.size() != 0) {
      column->validity.ClearAllBits();
    }
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
    const std::shared_ptr<ColumnChunk>& column = s.columns[i];
    if (!types_[i]) {
      out.AddColumn(
          ColumnView::Variants(static_cast<const Variant*>(s.data[i])), column);
    } else {
      out.AddColumn(
          ColumnView::Reference(*types_[i], s.data[i], &column->validity),
          column);
    }
  }
  out.Compose(RowSelection::Range(0), count);
  out.SetCardinality(count);
  return true;
}

}  // namespace perfetto::trace_processor::exec
