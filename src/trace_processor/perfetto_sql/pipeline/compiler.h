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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_COMPILER_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_COMPILER_H_

#include <cstdint>
#include <functional>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/sqlite/sql_source.h"

struct SyntaqliteParser;

namespace perfetto::trace_processor::pipeline {

// Source text of a parse tree node, for error tracebacks.
using NodeSourceFn = std::function<SqlSource(uint32_t node)>;

// Compiles a parsed pipeline into a logical plan, resolving the source and
// column names against `catalog` and checking types where known. Runs while
// the parse tree is alive so unsupported queries fail at parse time.
//
// `pipeline` is the SYNTAQLITE_NODE_PERFETTO_PIPELINE node.
base::StatusOr<LogicalPlan> Compile(SyntaqliteParser*,
                                    uint32_t pipeline,
                                    const NodeSourceFn&,
                                    const Catalog&);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_COMPILER_H_
