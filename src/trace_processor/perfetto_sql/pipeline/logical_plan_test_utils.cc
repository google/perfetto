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
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/aggregate_stage.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/distinct.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_fill_gaps.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_flatten.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_intersect.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/order_by.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/tree_accumulate.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/where.h"

namespace perfetto::trace_processor::pipeline {
namespace {

// An aggregate as the plan shows it, e.g. `SUM(#3)`.
std::string AggregateString(const Aggregate& agg) {
  switch (agg.function) {
    case Aggregate::Function::kCountStar:
      return "COUNT(*)";
    case Aggregate::Function::kSum:
      return "SUM(#" + std::to_string(agg.column) + ")";
    case Aggregate::Function::kCount:
      return "COUNT(#" + std::to_string(agg.column) + ")";
    case Aggregate::Function::kMin:
      return "MIN(#" + std::to_string(agg.column) + ")";
    case Aggregate::Function::kMax:
      return "MAX(#" + std::to_string(agg.column) + ")";
  }
  return "";
}

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
  const PlanType& type = plan.columns()[id].type;
  if (type.is_null()) {
    out += ":null";
  } else if (type.storage()) {
    out += ":";
    out += TypeName(*type.storage());
  }
  return out;
}

// A WHERE predicate's operator, e.g. `=` in `#2 = 5`.
std::string OpString(const WherePredicate::Op& op) {
  if (op.Is<core::Eq>()) {
    return "=";
  }
  if (op.Is<core::Ne>()) {
    return "!=";
  }
  if (op.Is<core::Lt>()) {
    return "<";
  }
  if (op.Is<core::Le>()) {
    return "<=";
  }
  if (op.Is<core::Gt>()) {
    return ">";
  }
  if (op.Is<core::IsNull>()) {
    return "IS NULL";
  }
  if (op.Is<core::IsNotNull>()) {
    return "IS NOT NULL";
  }
  if (op.Is<core::In>()) {
    return "IN";
  }
  PERFETTO_CHECK(op.Is<core::Ge>());
  return ">=";
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
  static std::string IntervalFillGapsString(const LogicalPlan&,
                                            const PlanNode&);
  static std::string OrderByString(const OrderBy&);
  static std::string DistinctString(const Distinct&);
  static std::string WhereString(const Where&);
  static std::string AggregateStageString(const LogicalPlan&,
                                          const AggregateStage&);
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
    out +=
        ", " + AggregateString(agg) + " -> " + ColumnString(plan, agg.output);
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
    out +=
        ", " + AggregateString(agg) + " -> " + ColumnString(plan, agg.output);
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
    const PlanNode& child = plan.nodes()[node.children()[i]];
    if (child.Is<Where>()) {
      out += ")\n    " +
             ScanString(plan, plan.nodes()[child.children()[0]].Cast<Scan>()) +
             "\n    " + WhereString(child.Cast<Where>());
    } else {
      out += ")\n    " + ScanString(plan, child.Cast<Scan>());
    }
  }
  return out;
}

std::string LogicalPlanFormatter::IntervalFillGapsString(
    const LogicalPlan& plan,
    const PlanNode& node) {
  const auto& fill = node.Cast<IntervalFillGaps>();
  std::string out = "IntervalFillGaps(ts=#" + std::to_string(fill.ts_) +
                    ", dur=#" + std::to_string(fill.dur_);
  for (ColumnId key : fill.keys_) {
    out += ", key=#" + std::to_string(key);
  }
  out += ")";
  for (const auto& output : fill.outputs_) {
    out += "\n  " + ColumnString(plan, output.column) + " <- ";
    out += "#" + std::to_string(output.input);
    out += " | ";
    out +=
        output.background ? "#" + std::to_string(*output.background) : "null";
  }
  out += "\n  background(ts=#" + std::to_string(fill.background_ts_) +
         ", dur=#" + std::to_string(fill.background_dur_);
  for (ColumnId key : fill.background_keys_) {
    out += ", key=#" + std::to_string(key);
  }
  return out + ")\n    " +
         ScanString(plan, plan.nodes()[node.children()[1]].Cast<Scan>());
}

std::string LogicalPlanFormatter::OrderByString(const OrderBy& order) {
  std::string out = "OrderBy(";
  for (size_t i = 0; i < order.keys_.size(); ++i) {
    out += (i ? ", #" : "#") + std::to_string(order.keys_[i].column) +
           (order.keys_[i].descending ? " DESC" : "");
  }
  return out + ")";
}

std::string LogicalPlanFormatter::DistinctString(const Distinct& distinct) {
  std::string out = "Distinct(";
  for (size_t i = 0; i < distinct.columns_.size(); ++i) {
    out += (i ? ", #" : "#") + std::to_string(distinct.columns_[i]);
  }
  return out + ")";
}

std::string LogicalPlanFormatter::WhereString(const Where& where) {
  std::string out = "Where(";
  for (size_t i = 0; i < where.predicates_.size(); ++i) {
    const WherePredicate& p = where.predicates_[i];
    out += (i ? ", #" : "#") + std::to_string(p.column) + " " + OpString(p.op);
    if (p.param) {
      out += " ?" + std::to_string(*p.param);
    } else if (!p.op.IsAnyOf<core::NullOp>()) {
      out += " " + (p.is_double ? std::to_string(p.double_value)
                                : std::to_string(p.int_value));
    }
  }
  return out + ")";
}

std::string LogicalPlanFormatter::AggregateStageString(
    const LogicalPlan& plan,
    const AggregateStage& aggregate) {
  std::string out = "Aggregate(";
  std::string separator;
  for (ColumnId key : aggregate.keys_) {
    out += separator + "key=#" + std::to_string(key);
    separator = ", ";
  }
  for (const auto& agg : aggregate.aggregates_) {
    out += separator + AggregateString(agg) + " -> " +
           ColumnString(plan, agg.output);
    separator = ", ";
  }
  return out + ")";
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
  if (node.Is<IntervalFillGaps>()) {
    return SubtreeString(plan, node.children()[0]) +
           IntervalFillGapsString(plan, node) + "\n";
  }
  if (node.Is<IntervalFlatten>()) {
    return SubtreeString(plan, node.children()[0]) +
           IntervalFlattenString(plan, node.Cast<IntervalFlatten>()) + "\n";
  }
  if (node.Is<AggregateStage>()) {
    return SubtreeString(plan, node.children()[0]) +
           AggregateStageString(plan, node.Cast<AggregateStage>()) + "\n";
  }
  if (node.Is<OrderBy>()) {
    return SubtreeString(plan, node.children()[0]) +
           OrderByString(node.Cast<OrderBy>()) + "\n";
  }
  if (node.Is<Distinct>()) {
    return SubtreeString(plan, node.children()[0]) +
           DistinctString(node.Cast<Distinct>()) + "\n";
  }
  if (node.Is<Where>()) {
    return SubtreeString(plan, node.children()[0]) +
           WhereString(node.Cast<Where>()) + "\n";
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
