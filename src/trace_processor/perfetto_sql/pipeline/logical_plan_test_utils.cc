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
#include <string>
#include <variant>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_flatten.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_intersect.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/tree_accumulate.h"

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
  if (plan.columns()[id].type) {
    out += ":";
    out += TypeName(*plan.columns()[id].type);
  }
  return out;
}

}  // namespace

// Kept in the test-only target: formatting must not add production binary size.
class LogicalPlanFormatter {
 public:
  static std::string Format(const LogicalPlan&);

 private:
  static std::string ScanString(const LogicalPlan&, const Scan&);
  static std::string TreeAccumulateString(const LogicalPlan&,
                                          const TreeAccumulate&);
  static std::string IntervalFlattenString(const LogicalPlan&,
                                           const IntervalFlatten&);
  static std::string IntervalIntersectString(const LogicalPlan&,
                                             const PlanNode&);
  static std::string SubtreeString(const LogicalPlan&, PlanNodeId);
};

std::string LogicalPlanFormatter::ScanString(const LogicalPlan& plan,
                                             const Scan& scan) {
  std::string out = "Scan(";
  switch (scan.source_.index()) {
    case base::variant_index<Scan::Source, Scan::Dataframe>():
      out += "table " + base::unchecked_get<Scan::Dataframe>(scan.source_).name;
      break;
    case base::variant_index<Scan::Source, SqlSource>():
      out += "sql " + base::unchecked_get<SqlSource>(scan.source_).sql();
      break;
    case base::variant_index<Scan::Source, Scan::DataframeArg>():
      out += "argument " +
             std::to_string(
                 base::unchecked_get<Scan::DataframeArg>(scan.source_).index);
      break;
    default:
      PERFETTO_FATAL("Unknown scan source");
  }
  out += ") [";
  for (size_t i = 0; i < scan.columns_.size(); ++i) {
    if (i) {
      out += ", ";
    }
    out += ColumnString(plan, scan.columns_[i].id) + " AS " +
           scan.columns_[i].name;
  }
  return out + "]";
}

std::string LogicalPlanFormatter::TreeAccumulateString(
    const LogicalPlan& plan,
    const TreeAccumulate& acc) {
  std::string out = "TreeAccumulate(";
  out += acc.direction_ == TreeDirection::kUp ? "up" : "down";
  out += ", node=#" + std::to_string(acc.node_column_);
  out += ", parent=#" + std::to_string(acc.parent_column_);
  for (const auto& agg : acc.aggregates_) {
    PERFETTO_DCHECK(agg.function == TreeAccumulate::Function::kSum);
    out += ", SUM(#" + std::to_string(agg.column) + ") -> " +
           ColumnString(plan, agg.output);
  }
  return out + ")";
}

std::string LogicalPlanFormatter::IntervalFlattenString(
    const LogicalPlan& plan,
    const IntervalFlatten& flatten) {
  std::string out = "IntervalFlatten(ts=#" + std::to_string(flatten.ts_) +
                    ", dur=#" + std::to_string(flatten.dur_);
  for (ColumnId key : flatten.keys_) {
    out += ", key=#" + std::to_string(key);
  }
  for (const auto& agg : flatten.aggregates_) {
    switch (agg.function) {
      case IntervalFlatten::Function::kCount:
        out += ", COUNT(*)";
        break;
      case IntervalFlatten::Function::kSum:
        out += ", SUM(#" + std::to_string(agg.column) + ")";
        break;
    }
    out += " -> " + ColumnString(plan, agg.output);
  }
  return out + ") -> " + ColumnString(plan, flatten.out_ts_) + ", " +
         ColumnString(plan, flatten.out_dur_);
}

std::string LogicalPlanFormatter::IntervalIntersectString(
    const LogicalPlan& plan,
    const PlanNode& node) {
  const auto& ii = node.Cast<IntervalIntersect>();
  std::string out = "IntervalIntersect(ts=#" + std::to_string(ii.ts_) +
                    ", dur=#" + std::to_string(ii.dur_) + ")";
  for (uint32_t i = 0; i < ii.operands_.size(); i++) {
    const IntervalIntersect::Operand& operand = ii.operands_[i];
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
           ScanString(plan, plan.nodes()[node.children()[i]].Cast<Scan>());
  }
  return out;
}

// Prints sources first so a pipeline reads in the order it runs.
// Intersection operands are printed inline.
std::string LogicalPlanFormatter::SubtreeString(const LogicalPlan& plan,
                                                PlanNodeId id) {
  const PlanNode& node = plan.nodes()[id];
  if (node.Is<Scan>()) {
    return ScanString(plan, node.Cast<Scan>()) + "\n";
  }
  if (node.Is<TreeAccumulate>()) {
    return SubtreeString(plan, node.children()[0]) +
           TreeAccumulateString(plan, node.Cast<TreeAccumulate>()) + "\n";
  }
  if (node.Is<IntervalIntersect>()) {
    return IntervalIntersectString(plan, node) + "\n";
  }
  if (node.Is<IntervalFlatten>()) {
    return SubtreeString(plan, node.children()[0]) +
           IntervalFlattenString(plan, node.Cast<IntervalFlatten>()) + "\n";
  }
  PERFETTO_FATAL("Unknown plan operation");
}

std::string LogicalPlanFormatter::Format(const LogicalPlan& plan) {
  std::string out = SubtreeString(plan, plan.root());
  out += "Output(";
  for (size_t i = 0; i < plan.output().size(); ++i) {
    if (i) {
      out += ", ";
    }
    out += "#" + std::to_string(plan.output()[i].id) + " AS " +
           plan.output()[i].name;
  }
  return out + ")\n";
}

std::string LogicalPlanToString(const LogicalPlan& plan) {
  return LogicalPlanFormatter::Format(plan);
}

}  // namespace perfetto::trace_processor::pipeline
