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

#include "src/trace_processor/perfetto_sql/lineage/connection_catalog.h"

#include <sqlite3.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/lineage/type_mapping.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor::lineage {
namespace {

// Finds the stored CREATE VIEW SQL for the view named $name. Temporary views
// shadow database views of the same name, so their rows sort first. SQL
// identifiers are case-insensitive but sqlite_master stores names with the
// case the user typed at CREATE time, so both sides are lowercased.
constexpr char kFindViewSql[] = R"(
  SELECT sql FROM (
    SELECT sql, 0 AS priority FROM sqlite_temp_master
    WHERE type = 'view' AND lower(name) = lower($name)
    UNION ALL
    SELECT sql, 1 AS priority FROM sqlite_master
    WHERE type = 'view' AND lower(name) = lower($name)
  )
  ORDER BY priority
  LIMIT 1
)";

// Escapes |name| as a single-quoted SQL string literal.
std::string Quoted(std::string_view name) {
  std::string out = "'";
  for (char c : name) {
    if (c == '\'') {
      out.push_back('\'');
    }
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

}  // namespace

ConnectionCatalog::ConnectionCatalog(PerfettoSqlConnection* connection)
    : connection_(connection) {}

std::optional<analysis::LeafRelation> ConnectionCatalog::FindLeafRelation(
    std::string_view name) const {
  const dataframe::Dataframe* dataframe = connection_->GetDataframeOrNull(name);
  if (!dataframe) {
    return std::nullopt;
  }
  analysis::LeafRelation relation;
  relation.name = name;
  const std::vector<std::string>& columns = dataframe->column_names();
  relation.columns.reserve(columns.size());
  for (uint32_t i = 0; i < columns.size(); ++i) {
    relation.columns.push_back(
        {columns[i], ToAnalysisType(dataframe->column_type(i))});
  }
  return relation;
}

std::optional<std::string> ConnectionCatalog::FindViewSql(
    std::string_view name) const {
  auto res = connection_->ExecuteUntilLastStatement(
      SqlSource::FromTraceProcessorImplementation(
          base::ReplaceAll(kFindViewSql, "$name", Quoted(name))));
  if (!res.ok() || res->stmt.IsDone()) {
    return std::nullopt;
  }
  sqlite3_stmt* stmt = res->stmt.sqlite_stmt();
  const auto* sql = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  return sql ? std::make_optional(std::string(sql)) : std::nullopt;
}

}  // namespace perfetto::trace_processor::lineage
