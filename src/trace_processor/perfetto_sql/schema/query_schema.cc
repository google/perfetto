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

#include "src/trace_processor/perfetto_sql/schema/query_schema.h"

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "perfetto/ext/base/status_macros.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/perfetto_sql/schema/type_mapping.h"
#include "src/trace_processor/sqlite/bindings/sqlite_column.h"

namespace perfetto::trace_processor::sql_schema {
namespace {

using core::StorageType;

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
    StorageType storage = ToStorageType(*type);
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

base::StatusOr<core::Schema> DescribeQuery(SqliteConnection* connection,
                                           const SqlSource& sql,
                                           const analysis::Catalog& catalog) {
  // Prepared here only to read the column names, then discarded: a statement
  // belongs to one execution, but the columns belong to the query.
  SqliteConnection::PreparedStatement statement =
      connection->PrepareStatement(sql);
  RETURN_IF_ERROR(statement.status());

  sqlite3_stmt* stmt = statement.sqlite_stmt();
  uint32_t count = sqlite::column::Count(stmt);
  std::vector<std::optional<StorageType>> types =
      ResolveTypes(sql, count, catalog);
  core::Schema columns;
  columns.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const char* name = sqlite::column::Name(stmt, i);
    columns.push_back({name ? name : "", types[i]});
  }
  return columns;
}

}  // namespace perfetto::trace_processor::sql_schema
