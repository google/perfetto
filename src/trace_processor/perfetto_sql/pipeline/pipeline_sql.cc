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

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/core/common/op_types.h"
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

// `value` as SQL reads it back: exactly, and a float as a float.
std::string ValueSql(const op::FilterValue& value) {
  switch (value.index()) {
    case base::variant_index<op::FilterValue, int64_t>():
      return std::to_string(base::unchecked_get<int64_t>(value));
    case base::variant_index<op::FilterValue, double>(): {
      double d = base::unchecked_get<double>(value);
      if (std::isinf(d)) {
        // Too large for a double, which SQL reads as infinity.
        return d > 0 ? "9e999" : "-9e999";
      }
      char buf[32];
      snprintf(buf, sizeof(buf), "%.17g", d);
      std::string text(buf);
      if (text.find_first_of(".e") == std::string::npos) {
        text += ".0";
      }
      return text;
    }
    case base::variant_index<op::FilterValue, std::string>():
      return QuoteString(base::unchecked_get<std::string>(value));
    default:
      // SQL sources are moved into SQL when the plan is written, before any
      // parameter is added.
      PERFETTO_FATAL("Unknown filter value");
  }
}

// `condition` as SQL, testing the column called `column`.
std::string ConditionSql(const op::FilterCondition& condition,
                         const std::string& column) {
  std::string out = QuoteIdentifier(column);
  switch (condition.op.index()) {
    case core::Op::GetTypeIndex<core::Eq>():
      return out + " = " + ValueSql(condition.values[0]);
    case core::Op::GetTypeIndex<core::Ne>():
      return out + " != " + ValueSql(condition.values[0]);
    case core::Op::GetTypeIndex<core::Lt>():
      return out + " < " + ValueSql(condition.values[0]);
    case core::Op::GetTypeIndex<core::Le>():
      return out + " <= " + ValueSql(condition.values[0]);
    case core::Op::GetTypeIndex<core::Gt>():
      return out + " > " + ValueSql(condition.values[0]);
    case core::Op::GetTypeIndex<core::Ge>():
      return out + " >= " + ValueSql(condition.values[0]);
    case core::Op::GetTypeIndex<core::IsNull>():
      return out + " IS NULL";
    case core::Op::GetTypeIndex<core::IsNotNull>():
      return out + " IS NOT NULL";
    case core::Op::GetTypeIndex<core::In>(): {
      std::vector<std::string> values;
      for (const op::FilterValue& value : condition.values) {
        values.push_back(ValueSql(value));
      }
      return out + " IN (" + base::Join(values, ", ") + ")";
    }
    default:
      PERFETTO_FATAL("Unknown filter operator");
  }
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
    std::string arg = "SELECT " + std::string(kDataframeAggFunction) + "(" +
                      QuoteString(base::Join(names, ",")) + ", " +
                      base::Join(references, ", ") + ") FROM " + from;
    // SQLite applies the scan's filters as it reads the relation, so the
    // dataframe only ever holds the rows kept. They mean the same there: the
    // dataframe's filters compare values as SQL does.
    std::vector<std::string> conditions;
    for (const op::FilterCondition& condition : scan.filters) {
      for (const NamedColumn& column : scan.columns) {
        if (column.id == condition.column) {
          conditions.push_back(ConditionSql(condition, column.name));
        }
      }
    }
    if (!conditions.empty()) {
      arg += " WHERE " + base::Join(conditions, " AND ");
    }
    scan.filters.clear();
    out.args.push_back(std::move(arg));
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
