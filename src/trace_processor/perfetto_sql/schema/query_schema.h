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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_SCHEMA_QUERY_SCHEMA_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_SCHEMA_QUERY_SCHEMA_H_

#include <optional>
#include <string>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/core/common/schema.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"

namespace perfetto::trace_processor::sql_schema {

// Where a query's rows come from, when they are exactly the rows of one leaf
// relation: a query which only picks and renames columns, however many views
// and subqueries that goes through.
struct RowOrigin {
  std::string relation;
  // The name of each result column and the leaf column it is, in result
  // order.
  struct Column {
    std::string name;
    std::string leaf_column;
  };
  std::vector<Column> columns;
};

struct DescribeOptions {
  // The query already parsed, analysed in place of the SQL text. It must
  // describe the same columns as the text does.
  std::optional<perfetto_sql::analysis::SqlNode> parsed;
  // Whether to trace the columns to where they come from at all, which walks
  // the definition of every view the query reads.
  bool lineage = true;
};

struct QueryDescription {
  // SQLite supplies the names; lineage supplies a type where a column traces
  // back to a dataframe column, else the column carries a type per row.
  core::Schema columns;
  std::optional<RowOrigin> row_origin;
  // The statement prepared to read the names, which can run the query once.
  SqliteConnection::PreparedStatement statement;
};

// Fails if SQLite cannot prepare the query.
base::StatusOr<QueryDescription> DescribeQuery(
    SqliteConnection*,
    const SqlSource&,
    const perfetto_sql::analysis::Catalog&,
    const DescribeOptions& = {});

}  // namespace perfetto::trace_processor::sql_schema

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_SCHEMA_QUERY_SCHEMA_H_
