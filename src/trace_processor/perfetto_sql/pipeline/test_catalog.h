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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_TEST_CATALOG_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_TEST_CATALOG_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/variant.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/runtime_dataframe_builder.h"
#include "src/trace_processor/perfetto_sql/engine/sqlite_dataframe_builder.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/schema/type_mapping.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor::pipeline {

// Catalog over dataframes built by the test. SQLite does not know about them.
// Anything else is looked up in `connection` (if given) and is untyped.
class TestCatalog : public Catalog {
 public:
  explicit TestCatalog(StringPool* pool, SqliteConnection* connection = nullptr)
      : pool_(pool), connection_(connection) {}

  // Adds a finalized table of int64 columns. nullopt is null.
  void AddTable(const std::string& name,
                std::vector<std::string> columns,
                const std::vector<std::vector<std::optional<int64_t>>>& rows) {
    dataframe::AdhocDataframeBuilder::Options options;
    options.types.assign(columns.size(),
                         dataframe::AdhocDataframeBuilder::ColumnType::kInt64);
    dataframe::AdhocDataframeBuilder builder(std::move(columns), pool_,
                                             options);
    for (const auto& row : rows) {
      for (uint32_t i = 0; i < row.size(); ++i) {
        if (row[i]) {
          PERFETTO_CHECK(builder.PushNonNull(i, *row[i]));
        } else {
          builder.PushNull(i);
        }
      }
    }
    auto df = std::move(builder).Build();
    PERFETTO_CHECK(df.ok());
    dataframes_[name] = std::make_unique<dataframe::Dataframe>(std::move(*df));
  }

  // Drops a table, as if replaced.
  void RemoveTable(const std::string& name) { dataframes_.Erase(name); }

  const dataframe::Dataframe* FindDataframe(
      std::string_view name) const override {
    auto* dataframe = dataframes_.Find(std::string(name));
    return dataframe ? dataframe->get() : nullptr;
  }

  std::optional<perfetto_sql::analysis::LeafRelation> FindLeafRelation(
      std::string_view name) const override {
    const dataframe::Dataframe* dataframe = FindDataframe(name);
    if (!dataframe) {
      if (!connection_) {
        return std::nullopt;
      }
      perfetto_sql::analysis::LeafRelation relation{std::string(name), {}};
      for (const auto& column :
           sqlite::utils::GetColumns(connection_->db(), relation.name)) {
        relation.columns.push_back({column.name, std::nullopt, column.hidden});
      }
      if (relation.columns.empty()) {
        return std::nullopt;
      }
      return relation;
    }
    perfetto_sql::analysis::LeafRelation relation;
    relation.name = std::string(name);
    const std::vector<std::string>& columns = dataframe->column_names();
    for (uint32_t i = 0; i < columns.size(); ++i) {
      relation.columns.push_back(
          {columns[i], sql_schema::ToAnalysisType(dataframe->column_type(i)),
           dataframe::IsHiddenColumn(columns[i])});
    }
    return relation;
  }
  std::optional<std::string> FindViewSql(std::string_view) const override {
    return std::nullopt;
  }

  // Materializes SQL inputs for execution tests, in dataframe argument order.
  static base::StatusOr<std::vector<std::unique_ptr<dataframe::Dataframe>>>
  BuildSqlSources(SqliteConnection*, StringPool*, const LogicalPlan&);

 private:
  StringPool* pool_;
  SqliteConnection* connection_;
  base::FlatHashMap<std::string, std::unique_ptr<dataframe::Dataframe>>
      dataframes_;
};

// A dataframe of each of `plan`'s SQL sources, built from `connection` as
// SQLite builds them where the pipeline is written. Once the SQL sources are
// moved out, the plan reads the i-th as dataframe argument i.
inline base::StatusOr<std::vector<std::unique_ptr<dataframe::Dataframe>>>
TestCatalog::BuildSqlSources(SqliteConnection* connection,
                             StringPool* pool,
                             const LogicalPlan& plan) {
  std::vector<std::unique_ptr<dataframe::Dataframe>> out;
  for (const PlanNode& node : plan.nodes()) {
    if (!node.Is<Scan>()) {
      continue;
    }
    const auto& scan = node.Cast<Scan>();
    if (!std::holds_alternative<SqlSource>(scan.source_)) {
      continue;
    }
    std::vector<std::string> names;
    std::vector<std::string> references;
    for (const NamedColumn& column : scan.columns_) {
      names.push_back(column.name);
      references.push_back("\"" + column.name + "\"");
    }
    auto statement = connection->PrepareStatement(SqlSource::FromExecuteQuery(
        "SELECT " + base::Join(references, ", ") + " FROM " +
        base::unchecked_get<SqlSource>(scan.source_).sql()));
    statement.Step();
    RETURN_IF_ERROR(statement.status());
    ASSIGN_OR_RETURN(dataframe::RuntimeDataframeBuilder builder,
                     BuildRuntimeDataframeFromSqliteStatement(
                         pool, std::move(names), &statement, "SQL source"));
    ASSIGN_OR_RETURN(dataframe::Dataframe dataframe,
                     std::move(builder).Build());
    out.push_back(std::make_unique<dataframe::Dataframe>(std::move(dataframe)));
  }
  return std::move(out);
}

// `dataframes`, as the arguments to pass a plan.
inline std::vector<const dataframe::Dataframe*> DataframeArgs(
    const std::vector<std::unique_ptr<dataframe::Dataframe>>& dataframes) {
  std::vector<const dataframe::Dataframe*> args;
  for (const auto& dataframe : dataframes) {
    args.push_back(dataframe.get());
  }
  return args;
}

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_TEST_CATALOG_H_
