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

#include "src/trace_processor/perfetto_sql/schema/sqlite_relations.h"

#include <optional>
#include <string>
#include <string_view>

#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/sqlite/bindings/sqlite_column.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"

namespace perfetto::trace_processor::sql_schema {

std::optional<perfetto_sql::analysis::LeafRelation> FindSqliteRelation(
    SqliteConnection* connection,
    std::string_view name) {
  std::string quoted =
      "'" + base::ReplaceAll(std::string(name), "'", "''") + "'";
  SqliteConnection::PreparedStatement stmt =
      connection->PrepareStatement(SqlSource::FromTraceProcessorImplementation(
          "SELECT name, hidden FROM pragma_table_xinfo(" + quoted + ")"));
  perfetto_sql::analysis::LeafRelation relation;
  relation.name = std::string(name);
  while (stmt.Step()) {
    const char* column = sqlite::column::Text(stmt.sqlite_stmt(), 0);
    // 1 is a hidden column; 2 and 3 are generated ones, which `*` includes.
    bool hidden = sqlite::column::Int64(stmt.sqlite_stmt(), 1) == 1;
    relation.columns.push_back({column ? column : "", std::nullopt, hidden});
  }
  if (!stmt.status().ok() || relation.columns.empty()) {
    return std::nullopt;
  }
  return relation;
}

}  // namespace perfetto::trace_processor::sql_schema
