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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_SYNTAX_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_SYNTAX_H_

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor::pipeline {

// `|> TREE ACCUMULATE UP | DOWN agg AS name, ...`
struct TreeAccumulate {
  enum class Direction { kUp, kDown };
  struct Aggregate {
    // Upper case, as written.
    std::string function;
    // The column being aggregated, or nothing for `*`.
    std::optional<std::string> column;
    std::string name;
    SqlSource sql;
  };
  Direction direction = Direction::kUp;
  std::vector<Aggregate> aggregates;
};

struct Stage {
  std::variant<TreeAccumulate> op;
  SqlSource sql;
};

// A pipeline as written: `FROM from |> stage |> stage ...`.
struct PipelineSyntax {
  // Everything between FROM and the first stage. Anything a SQL FROM clause
  // accepts goes here, so it is kept as text for SQLite.
  SqlSource from;
  // The table named, when the FROM clause is nothing but a name.
  std::optional<std::string> from_name;
  std::vector<Stage> stages;
};

// Parses a pipeline, which must start with FROM.
//
// Only the stages are parsed. What they refer to, and whether that exists, is
// for whoever plans the pipeline.
base::StatusOr<PipelineSyntax> ParsePipeline(SqlSource);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_SYNTAX_H_
