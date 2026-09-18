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

#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/core/common/schema.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"

namespace perfetto::trace_processor::sql_schema {

// SQLite supplies result names; semantic analysis supplies types where known.
// Fails if SQLite cannot prepare the query. Unknown types remain per-row
// variants.
base::StatusOr<core::Schema> DescribeQuery(
    SqliteConnection*,
    const SqlSource&,
    const perfetto_sql::analysis::Catalog&);

}  // namespace perfetto::trace_processor::sql_schema

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_SCHEMA_QUERY_SCHEMA_H_
