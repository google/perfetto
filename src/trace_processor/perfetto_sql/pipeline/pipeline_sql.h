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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_SQL_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_SQL_H_

#include <cstdint>
#include <functional>
#include <string>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/prepared_statement.h"
#include "src/trace_processor/sqlite/sql_source.h"

struct SyntaqliteParser;

namespace perfetto::trace_processor::pipeline {

// -----------------------------------------------------------------------------
// PipeSQL compilation entry point
// -----------------------------------------------------------------------------

// Maps a syntaqlite AST node ID to its SqlSource span for compiler
// diagnostics.
using NodeSourceFn = std::function<SqlSource(uint32_t node)>;

// Compiles a parsed PipeSQL pipeline AST node into a PreparedStatement via
// `pipeline::Builder`, resolving sources and column names against `catalog`.
// Runs while the parse tree is alive so unsupported queries fail at parse time.
//
// `pipeline` is a SYNTAQLITE_NODE_PERFETTO_PIPELINE node ID.
base::StatusOr<PreparedStatement> Compile(SyntaqliteParser* parser,
                                          uint32_t pipeline,
                                          const NodeSourceFn& source,
                                          const Catalog& catalog);

// -----------------------------------------------------------------------------
// SQLite intrinsic function names
// -----------------------------------------------------------------------------

// Maximum number of output columns supported by the `__intrinsic_pipeline`
// SQLite table function.
inline constexpr uint32_t kMaxPipelineColumns = 256;

// The table function which runs a serialized statement. It outputs at most
// kMaxPipelineColumns columns.
inline constexpr char kPipelineFunction[] = "__intrinsic_pipeline";

// The aggregate which builds a dataframe of a relation's rows, for a pipeline
// to read: `__intrinsic_dataframe_agg('a,b', a, b)`.
inline constexpr char kDataframeAggFunction[] = "__intrinsic_dataframe_agg";

// The function which gathers the dataframes a pipeline reads into the one
// argument the table function takes them in:
// `__intrinsic_dataframes((SELECT ...), (SELECT ...))`.
inline constexpr char kDataframesFunction[] = "__intrinsic_dataframes";

// -----------------------------------------------------------------------------
// SQL quoting
// -----------------------------------------------------------------------------

std::string QuoteIdentifier(const std::string& name);

std::string QuoteString(const std::string& text);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_SQL_H_
