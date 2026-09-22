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

#include "src/trace_processor/perfetto_sql/engine/connection_catalog.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/schema/query_schema.h"
#include "src/trace_processor/perfetto_sql/schema/type_mapping.h"
#include "src/trace_processor/sqlite/bindings/sqlite_column.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"

namespace perfetto::trace_processor {
namespace {

namespace analysis = ::perfetto::perfetto_sql::analysis;

// Finds the stored CREATE VIEW SQL for the view named $name. Temporary views
// shadow database views of the same name, so their rows sort first. SQL
// identifiers are case-insensitive but sqlite_master stores names with the
// case the user typed at CREATE time, so both sides are lowercased.
constexpr char kFindViewSql[] = R"(
  SELECT sql FROM (
    SELECT sql, 0 AS priority FROM sqlite_temp_master
    WHERE type = 'view' AND lower(name) = lower(?1)
    UNION ALL
    SELECT sql, 1 AS priority FROM sqlite_master
    WHERE type = 'view' AND lower(name) = lower(?1)
  )
  ORDER BY priority
  LIMIT 1
)";

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
        {columns[i], sql_schema::ToAnalysisType(dataframe->column_type(i))});
  }
  return relation;
}

std::optional<std::string> ConnectionCatalog::FindViewSql(
    std::string_view name) const {
  if (!find_view_) {
    find_view_.emplace(connection_->sqlite_connection()->PrepareStatement(
        SqlSource::FromTraceProcessorImplementation(kFindViewSql)));
  }
  sqlite3_stmt* stmt = find_view_->sqlite_stmt();
  if (!stmt ||
      sqlite3_bind_text(stmt, 1, name.data(), static_cast<int>(name.size()),
                        SQLITE_STATIC) != SQLITE_OK) {
    return std::nullopt;
  }
  std::optional<std::string> sql;
  if (find_view_->Step()) {
    if (const char* text = sqlite::column::Text(stmt, 0)) {
      sql = text;
    }
  }
  // Also lets go of `name`, which the statement only borrowed.
  find_view_->Reset();
  sqlite3_clear_bindings(stmt);
  return sql;
}

const dataframe::Dataframe* ConnectionCatalog::FindDataframe(
    std::string_view name) const {
  return connection_->GetDataframeOrNull(name);
}

base::StatusOr<pipeline::Catalog::QueryDescription>
ConnectionCatalog::DescribeQuery(
    const SqlSource& sql,
    const sql_schema::DescribeOptions& options) const {
  ASSIGN_OR_RETURN(sql_schema::QueryDescription described,
                   sql_schema::DescribeQuery(connection_->sqlite_connection(),
                                             sql, *this, options));
  QueryDescription out;
  out.columns = std::move(described.columns);
  out.statement = std::make_shared<SqliteConnection::PreparedStatement>(
      std::move(described.statement));
  std::optional<sql_schema::RowOrigin>& origin = described.row_origin;
  if (!origin) {
    return out;
  }
  DataframeColumns found;
  found.name = origin->relation;
  found.dataframe = connection_->GetDataframeOrNull(origin->relation);
  if (!found.dataframe) {
    return out;
  }
  const std::vector<std::string>& names = found.dataframe->column_names();
  for (const sql_schema::RowOrigin::Column& column : origin->columns) {
    auto it = std::find_if(names.begin(), names.end(), [&](const auto& name) {
      return base::CaseInsensitiveEqual(name, column.leaf_column);
    });
    if (it == names.end()) {
      return out;
    }
    found.columns.push_back(
        {column.name, static_cast<uint32_t>(it - names.begin())});
  }
  out.dataframe = std::move(found);
  return out;
}

}  // namespace perfetto::trace_processor
