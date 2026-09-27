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
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/exec/collected_rows.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/schema/sqlite_relations.h"
#include "src/trace_processor/perfetto_sql/schema/type_mapping.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"

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
      return connection_ ? sql_schema::FindSqliteRelation(connection_, name)
                         : std::nullopt;
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

 private:
  StringPool* pool_;
  SqliteConnection* connection_;
  base::FlatHashMap<std::string, std::unique_ptr<dataframe::Dataframe>>
      dataframes_;
};

// Collects the rows of each of `plan`'s SQL sources from `connection`, as
// SQLite does where the pipeline is written: the inputs a plan with its SQL
// sources moved out reads.
inline base::StatusOr<exec::CollectedRowsScan::Inputs> CollectSqlInputs(
    SqliteConnection* connection,
    StringPool* pool,
    const LogicalPlan& plan) {
  exec::CollectedRowsScan::Inputs inputs;
  for (const PlanNode& node : plan.nodes) {
    if (!node.Is<op::Scan>()) {
      continue;
    }
    const auto& scan = node.Cast<op::Scan>();
    if (!std::holds_alternative<SqlSource>(scan.source)) {
      continue;
    }
    core::Schema columns;
    std::vector<std::string> names;
    for (const NamedColumn& column : scan.columns) {
      columns.push_back({column.name, plan.columns[column.id].type});
      names.push_back("\"" + column.name + "\"");
    }
    auto rows = std::make_shared<exec::CollectedRows>(columns, pool);
    auto statement = connection->PrepareStatement(SqlSource::FromExecuteQuery(
        "SELECT " + base::Join(names, ", ") + " FROM " +
        base::unchecked_get<SqlSource>(scan.source).sql()));
    while (statement.Step()) {
      std::vector<sqlite3_value*> values;
      for (uint32_t i = 0; i < columns.size(); ++i) {
        values.push_back(
            sqlite3_column_value(statement.sqlite_stmt(), static_cast<int>(i)));
      }
      RETURN_IF_ERROR(rows->Append(values.data()));
    }
    RETURN_IF_ERROR(statement.status());
    inputs.push_back(std::move(rows));
  }
  return inputs;
}

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_TEST_CATALOG_H_
