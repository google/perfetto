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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PFGRAPH_PFGRAPH_COMPILER_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PFGRAPH_PFGRAPH_COMPILER_H_

#include <string>
#include <string_view>

#include "perfetto/ext/base/status_or.h"

namespace perfetto::trace_processor::pfgraph {

// Compiles a .pfgraph source text into PerfettoSQL statements.
// The output can be directly executed by PerfettoSqlEngine.
base::StatusOr<std::string> CompilePfGraph(std::string_view source);

}  // namespace perfetto::trace_processor::pfgraph

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PFGRAPH_PFGRAPH_COMPILER_H_
