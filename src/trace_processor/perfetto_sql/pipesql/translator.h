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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPESQL_TRANSLATOR_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPESQL_TRANSLATOR_H_

#include <cstdint>
#include <functional>

#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/prepared_statement.h"
#include "src/trace_processor/sqlite/sql_source.h"

struct SyntaqliteParser;

namespace perfetto::trace_processor::pipesql {

// Resolves tables and views during PipeSQL translation: both dataframes that
// can be read directly and SQL relations described via semantic analysis.
class Catalog : public pipeline::Catalog,
                public perfetto_sql::analysis::Catalog {
 public:
  ~Catalog() override;
};

// Maps a syntaqlite AST node ID to its SqlSource span for diagnostics.
using NodeSourceFn = std::function<SqlSource(uint32_t node)>;

// Translates a parsed SYNTAQLITE_NODE_PERFETTO_PIPELINE AST node into a
// pipeline::PreparedStatement, resolving sources and names against `catalog`.
base::StatusOr<pipeline::PreparedStatement> Translate(
    SyntaqliteParser* parser,
    uint32_t pipeline,
    const NodeSourceFn& source,
    const Catalog& catalog);

}  // namespace perfetto::trace_processor::pipesql

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPESQL_TRANSLATOR_H_
