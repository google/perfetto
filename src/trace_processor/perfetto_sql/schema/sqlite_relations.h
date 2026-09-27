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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_SCHEMA_SQLITE_RELATIONS_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_SCHEMA_SQLITE_RELATIONS_H_

#include <optional>
#include <string_view>

#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"

namespace perfetto::trace_processor::sql_schema {

// The columns of the table, virtual table or table function SQLite knows as
// `name`, or nothing when it knows none. They are untyped: SQLite's declared
// types establish nothing, as an INTEGER column holds text if something puts
// text in it.
std::optional<perfetto_sql::analysis::LeafRelation> FindSqliteRelation(
    SqliteConnection*,
    std::string_view name);

}  // namespace perfetto::trace_processor::sql_schema

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_SCHEMA_SQLITE_RELATIONS_H_
