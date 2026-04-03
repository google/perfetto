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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PFGRAPH_PFGRAPH_YAML_TO_AST_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PFGRAPH_PFGRAPH_YAML_TO_AST_H_

#include <string_view>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/perfetto_sql/pfgraph/pfgraph_ast.h"

namespace perfetto::trace_processor::pfgraph {

// Parses a YAML string into a PfGraph AST (GraphModule).
// This is the YAML equivalent of ParsePfGraph() — same AST output,
// different input format.
base::StatusOr<GraphModule> ParsePfGraphYaml(std::string_view yaml_input);

}  // namespace perfetto::trace_processor::pfgraph

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PFGRAPH_PFGRAPH_YAML_TO_AST_H_
