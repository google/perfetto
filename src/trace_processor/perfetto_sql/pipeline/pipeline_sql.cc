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
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor::pipeline {
namespace {

std::string QuoteIdentifier(const std::string& name) {
  return "\"" + base::ReplaceAll(name, "\"", "\"\"") + "\"";
}

std::string QuoteString(const std::string& text) {
  return "'" + base::ReplaceAll(text, "'", "''") + "'";
}

}  // namespace

PlanWithDataframeArgs MoveSqlSourcesToDataframeArgs(LogicalPlan plan) {
  PlanWithDataframeArgs out;
  for (PlanNode& node : plan.nodes) {
    if (!node.Is<op::Scan>()) {
      continue;
    }
    auto& scan = node.Cast<op::Scan>();
    if (!std::holds_alternative<SqlSource>(scan.source)) {
      continue;
    }
    std::vector<std::string> names;
    std::vector<std::string> references;
    for (const NamedColumn& column : scan.columns) {
      names.push_back(column.name);
      references.push_back(QuoteIdentifier(column.name));
    }
    const std::string& from = base::unchecked_get<SqlSource>(scan.source).sql();
    out.args.push_back("SELECT " + std::string(kDataframeAggFunction) + "(" +
                       QuoteString(base::Join(names, ",")) + ", " +
                       base::Join(references, ", ") + ") FROM " + from);
    scan.source =
        op::Scan::DataframeArg{static_cast<uint32_t>(out.args.size() - 1)};
  }
  out.plan = std::move(plan);
  return out;
}

base::StatusOr<std::string> SelectPipelineSql(const LogicalPlan& plan) {
  if (plan.output.size() > kMaxPipelineColumns) {
    return base::ErrStatus("A pipeline can output at most %u columns, not %zu",
                           kMaxPipelineColumns, plan.output.size());
  }
  PlanWithDataframeArgs moved = MoveSqlSourcesToDataframeArgs(plan);
  std::vector<std::string> columns;
  for (uint32_t i = 0; i < plan.output.size(); ++i) {
    columns.push_back("c" + std::to_string(i) + " AS " +
                      QuoteIdentifier(plan.output[i].name));
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

}  // namespace perfetto::trace_processor::pipeline
