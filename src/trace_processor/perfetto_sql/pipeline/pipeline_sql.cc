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

#include "src/trace_processor/perfetto_sql/pipeline/pipeline_sql.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {

PlanWithDataframeArgs MoveSqlSourcesToDataframeArgs(LogicalPlan plan) {
  PlanWithDataframeArgs out;
  for (PlanNode& node : plan.nodes()) {
    node.operation().MoveSqlSourcesToDataframeArgs(&out.args);
  }
  out.plan = std::move(plan);
  return out;
}

base::StatusOr<std::string> SelectPipelineSql(const LogicalPlan& plan) {
  if (plan.output().size() > kMaxPipelineColumns) {
    return base::ErrStatus("A pipeline can output at most %u columns, not %zu",
                           kMaxPipelineColumns, plan.output().size());
  }
  PlanWithDataframeArgs moved = MoveSqlSourcesToDataframeArgs(plan);
  std::vector<std::string> columns;
  for (uint32_t i = 0; i < plan.output().size(); ++i) {
    columns.push_back("c" + std::to_string(i) + " AS " +
                      QuoteIdentifier(plan.output()[i].name));
  }
  std::string arguments = "X'" + base::ToHex(SerializePlan(moved.plan)) + "'";
  if (!moved.args.empty()) {
    std::vector<std::string> dataframes;
    for (const std::string& arg : moved.args) {
      dataframes.push_back("(" + arg + ")");
    }
    // A subquery, so SQLite builds the list once rather than on every filter.
    arguments += ", (SELECT " + std::string(kDataframesFunction) + "(" +
                 base::Join(dataframes, ", ") + "))";
  }
  return "SELECT " + base::Join(columns, ", ") + " FROM " + kPipelineFunction +
         "(" + arguments + ")";
}

std::string QuoteIdentifier(const std::string& name) {
  return "\"" + base::ReplaceAll(name, "\"", "\"\"") + "\"";
}

std::string QuoteString(const std::string& text) {
  return "'" + base::ReplaceAll(text, "'", "''") + "'";
}

}  // namespace perfetto::trace_processor::pipeline
