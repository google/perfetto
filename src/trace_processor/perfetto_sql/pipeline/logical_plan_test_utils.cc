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

#include "src/trace_processor/perfetto_sql/pipeline/logical_plan_test_utils.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/core/common/storage_types.h"

namespace perfetto::trace_processor::pipeline {
namespace {

const char* TypeName(core::StorageType type) {
  switch (type.index()) {
    case core::StorageType::GetTypeIndex<core::Id>():
      return "id";
    case core::StorageType::GetTypeIndex<core::Uint32>():
      return "uint32";
    case core::StorageType::GetTypeIndex<core::Int32>():
      return "int32";
    case core::StorageType::GetTypeIndex<core::Int64>():
      return "int64";
    case core::StorageType::GetTypeIndex<core::Double>():
      return "double";
    case core::StorageType::GetTypeIndex<core::String>():
      return "string";
    default:
      PERFETTO_FATAL("Unknown storage type");
  }
}

std::string ColumnString(const LogicalPlan& plan, ColumnId id) {
  std::string out = "#" + std::to_string(id);
  if (plan.columns[id].type) {
    out += ":";
    out += TypeName(*plan.columns[id].type);
  }
  return out;
}

std::string ConditionsString(
    const std::vector<op::FilterCondition>& conditions) {
  static constexpr const char* kOps[] = {
      "=",    "!=",     "<",           "<=",      ">", ">=",
      "GLOB", "REGEXP", "IS NOT NULL", "IS NULL", "IN"};
  static_assert(std::size(kOps) == core::Op::kSize);
  std::string out;
  for (size_t i = 0; i < conditions.size(); ++i) {
    const op::FilterCondition& condition = conditions[i];
    out += (i ? " AND #" : "#") + std::to_string(condition.column) + " " +
           kOps[condition.op.index()];
    for (const op::FilterValue& value : condition.values) {
      switch (value.index()) {
        case base::variant_index<op::FilterValue, int64_t>():
          out += " " + std::to_string(base::unchecked_get<int64_t>(value));
          break;
        case base::variant_index<op::FilterValue, double>():
          out += " " + std::to_string(base::unchecked_get<double>(value));
          break;
        case base::variant_index<op::FilterValue, std::string>():
          out += " '" + base::unchecked_get<std::string>(value) + "'";
          break;
        default:
          PERFETTO_FATAL("Unknown filter value");
      }
    }
  }
  return out;
}

std::string FilterString(const op::Filter& filter) {
  return "Filter(" + ConditionsString(filter.conditions) + ")";
}

std::string ScanString(const LogicalPlan& plan, const op::Scan& scan) {
  std::string out = "Scan(";
  switch (scan.source.index()) {
    case base::variant_index<op::Scan::Source, op::Scan::Dataframe>():
      out +=
          "table " + base::unchecked_get<op::Scan::Dataframe>(scan.source).name;
      break;
    case base::variant_index<op::Scan::Source, SqlSource>():
      out += "sql " + base::unchecked_get<SqlSource>(scan.source).sql();
      break;
    case base::variant_index<op::Scan::Source, op::Scan::DataframeArg>():
      out +=
          "argument " +
          std::to_string(
              base::unchecked_get<op::Scan::DataframeArg>(scan.source).index);
      break;
    default:
      PERFETTO_FATAL("Unknown scan source");
  }
  out += ") [";
  for (size_t i = 0; i < scan.columns.size(); ++i) {
    if (i) {
      out += ", ";
    }
    out +=
        ColumnString(plan, scan.columns[i].id) + " AS " + scan.columns[i].name;
  }
  out += "]";
  if (!scan.filters.empty()) {
    out += " WHERE " + ConditionsString(scan.filters);
  }
  return out;
}

std::string TreeAccumulateString(const LogicalPlan& plan,
                                 const op::TreeAccumulate& acc) {
  std::string out = "TreeAccumulate(";
  out += acc.direction == op::TreeDirection::kUp ? "up" : "down";
  out += ", node=#" + std::to_string(acc.node_column);
  out += ", parent=#" + std::to_string(acc.parent_column);
  for (const auto& agg : acc.aggregates) {
    PERFETTO_DCHECK(agg.function == op::TreeAccumulate::Function::kSum);
    out += ", SUM(#" + std::to_string(agg.column) + ") -> " +
           ColumnString(plan, agg.output);
  }
  return out + ")";
}

std::string IntervalIntersectString(const LogicalPlan& plan,
                                    const PlanNode& node) {
  const auto& ii = node.Cast<op::IntervalIntersect>();
  std::string out = "IntervalIntersect(ts=#" + std::to_string(ii.ts) +
                    ", dur=#" + std::to_string(ii.dur) + ")";
  for (uint32_t i = 0; i < ii.operands.size(); i++) {
    const op::IntervalIntersect::Operand& operand = ii.operands[i];
    out += "\n  operand(ts=#" + std::to_string(operand.ts) + ", dur=#" +
           std::to_string(operand.dur);
    for (ColumnId key : operand.keys) {
      out += ", key=#" + std::to_string(key);
    }
    out += ", carries=[";
    for (uint32_t c = 0; c < operand.carried.size(); c++) {
      out += (c ? ", #" : "#") + std::to_string(operand.carried[c]);
    }
    out += "]";
    out += ")\n    " +
           ScanString(plan, plan.nodes[node.children[i]].Cast<op::Scan>());
  }
  return out;
}

// Prints sources first so a pipeline reads in the order it runs.
// Intersection operands are printed inline.
std::string SubtreeString(const LogicalPlan& plan, PlanNodeId id) {
  const PlanNode& node = plan.nodes[id];
  switch (node.op.index()) {
    case base::variant_index<Op, op::Scan>():
      return ScanString(plan, node.Cast<op::Scan>()) + "\n";
    case base::variant_index<Op, op::Filter>():
      return SubtreeString(plan, node.children[0]) +
             FilterString(node.Cast<op::Filter>()) + "\n";
    case base::variant_index<Op, op::TreeAccumulate>():
      return SubtreeString(plan, node.children[0]) +
             TreeAccumulateString(plan, node.Cast<op::TreeAccumulate>()) + "\n";
    case base::variant_index<Op, op::IntervalIntersect>():
      return IntervalIntersectString(plan, node) + "\n";
    default:
      PERFETTO_FATAL("Unknown operator");
  }
}

}  // namespace

std::string LogicalPlanToString(const LogicalPlan& plan) {
  std::string out = SubtreeString(plan, plan.root);
  out += "Output(";
  for (size_t i = 0; i < plan.output.size(); ++i) {
    if (i) {
      out += ", ";
    }
    out +=
        "#" + std::to_string(plan.output[i].id) + " AS " + plan.output[i].name;
  }
  return out + ")\n";
}

}  // namespace perfetto::trace_processor::pipeline
