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
#include <utility>
#include <vector>

#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/lineage/column_lineage.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor::lineage {
namespace {

std::string Quoted(const std::string& name) {
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

ConnectionCatalog::~ConnectionCatalog() = default;

const std::vector<ResolvedColumn>* ConnectionCatalog::Dataframe(
    const std::string& name) const {
  auto it = dataframes_.find(name);
  if (it != dataframes_.end()) {
    return it->second.empty() ? nullptr : &it->second;
  }
  std::vector<ResolvedColumn>& columns = dataframes_[name];
  const dataframe::Dataframe* df = connection_->GetDataframeOrNull(name);
  if (!df) {
    return nullptr;
  }
  for (uint32_t i = 0; i < df->column_count(); ++i) {
    ResolvedColumn column;
    column.name = df->column_names()[i];
    column.type = df->column_type(i);
    columns.push_back(std::move(column));
  }
  return columns.empty() ? nullptr : &columns;
}

std::optional<std::string> ConnectionCatalog::ViewSql(
    const std::string& name) const {
  auto res = connection_->ExecuteUntilLastStatement(SqlSource::FromExecuteQuery(
      "SELECT sql FROM sqlite_master WHERE type = 'view' AND name = " +
      Quoted(name)));
  if (!res.ok() || res->stmt.IsDone()) {
    return std::nullopt;
  }
  sqlite3_stmt* stmt = res->stmt.sqlite_stmt();
  const auto* sql = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  return sql ? std::make_optional(std::string(sql)) : std::nullopt;
}

}  // namespace perfetto::trace_processor::lineage
