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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_CATALOG_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_CATALOG_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/schema/query_schema.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor::pipeline {

// Lookup interface the compiler uses to resolve what a pipeline reads.
class Catalog {
 public:
  virtual ~Catalog();

  // Dataframe registered as `name`, or null.
  virtual const dataframe::Dataframe* FindDataframe(
      std::string_view name) const = 0;

  // The dataframe columns a query is made of, when all it does is pick and
  // rename the columns of one dataframe: every row of the query is then a row
  // of the dataframe, which can be read without going through SQLite.
  struct DataframeColumns {
    std::string name;
    const dataframe::Dataframe* dataframe = nullptr;
    // The name of each result column and the dataframe column it is, in
    // result order.
    struct Column {
      std::string name;
      uint32_t index;
    };
    std::vector<Column> columns;
  };
  struct QueryDescription {
    // Typed where a column traces back to a dataframe column.
    Schema columns;
    std::optional<DataframeColumns> dataframe;
    // Prepared to describe the query, and good for running it once.
    std::shared_ptr<SqliteConnection::PreparedStatement> statement;
  };
  // Describes the query `sql`. Fails if SQLite cannot prepare it.
  virtual base::StatusOr<QueryDescription> DescribeQuery(
      const SqlSource& sql,
      const sql_schema::DescribeOptions& options) const = 0;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_CATALOG_H_
