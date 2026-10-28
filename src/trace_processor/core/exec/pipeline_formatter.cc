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

#include "src/trace_processor/core/exec/pipeline_formatter.h"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/aggregate_function.h"
#include "src/trace_processor/core/exec/assert_type.h"
#include "src/trace_processor/core/exec/combine_batches.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/distinct.h"
#include "src/trace_processor/core/exec/expression.h"
#include "src/trace_processor/core/exec/filter.h"
#include "src/trace_processor/core/exec/graph_tree.h"
#include "src/trace_processor/core/exec/grouped_sort.h"
#include "src/trace_processor/core/exec/hash_aggregate.h"
#include "src/trace_processor/core/exec/index_scan.h"
#include "src/trace_processor/core/exec/interval_fill_gaps.h"
#include "src/trace_processor/core/exec/interval_flatten.h"
#include "src/trace_processor/core/exec/interval_intersect.h"
#include "src/trace_processor/core/exec/interval_lanes.h"
#include "src/trace_processor/core/exec/limit.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/sort.h"
#include "src/trace_processor/core/exec/sorted_rows.h"
#include "src/trace_processor/core/exec/sorted_scan.h"
#include "src/trace_processor/core/exec/tree_accumulate.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/exec/tree_order.h"

namespace perfetto::trace_processor::core::exec {
namespace {

std::string OpName(const Op& op) {
  switch (op.index()) {
    case Op::GetTypeIndex<Eq>():
      return "=";
    case Op::GetTypeIndex<Ne>():
      return "!=";
    case Op::GetTypeIndex<Lt>():
      return "<";
    case Op::GetTypeIndex<Le>():
      return "<=";
    case Op::GetTypeIndex<Gt>():
      return ">";
    case Op::GetTypeIndex<Ge>():
      return ">=";
    case Op::GetTypeIndex<In>():
      return "IN";
    case Op::GetTypeIndex<Glob>():
      return "GLOB";
    case Op::GetTypeIndex<IsNull>():
      return "IS NULL";
    case Op::GetTypeIndex<IsNotNull>():
      return "IS NOT NULL";
    default:
      return "?";
  }
}

std::string Column(uint32_t position) {
  return "#" + std::to_string(position);
}

std::string Columns(const std::vector<uint32_t>& positions) {
  std::string out;
  for (uint32_t position : positions) {
    out += (out.empty() ? "" : ", ") + Column(position);
  }
  return out;
}

std::string Lanes(const IntervalLaneSpec& lanes) {
  std::string out = Column(lanes.ts_column) + ", " + Column(lanes.dur_column);
  if (!lanes.key_columns.empty()) {
    out += ", keys: " + Columns(lanes.key_columns) +
           ", groups: " + Column(lanes.group_column);
  }
  return out;
}

std::string Keys(const std::vector<SortKey>& keys) {
  std::string out;
  for (const SortKey& key : keys) {
    out += (out.empty() ? "" : ", ") + Column(key.column) +
           (key.descending ? " desc" : "");
  }
  return out;
}

std::string Aggregates(const std::vector<AggregateCall>& calls) {
  std::string out;
  for (const AggregateCall& call : calls) {
    std::string arg = Column(call.column);
    std::string text;
    switch (call.function) {
      case AggregateCall::Function::kCountStar:
        text = "COUNT(*)";
        break;
      case AggregateCall::Function::kCount:
        text = "COUNT(" + arg + ")";
        break;
      case AggregateCall::Function::kSum:
        text = "SUM(" + arg + ")";
        break;
      case AggregateCall::Function::kMin:
        text = "MIN(" + arg + ")";
        break;
      case AggregateCall::Function::kMax:
        text = "MAX(" + arg + ")";
        break;
    }
    out += (out.empty() ? "" : ", ") + text;
  }
  return out;
}

std::string TypeName(const StorageType& type) {
  if (type.Is<Id>()) {
    return "Id";
  }
  if (type.Is<Uint32>()) {
    return "Uint32";
  }
  if (type.Is<Int32>()) {
    return "Int32";
  }
  if (type.Is<Int64>()) {
    return "Int64";
  }
  if (type.Is<Double>()) {
    return "Double";
  }
  return "String";
}

// Prints step `i` of `program` as an expression.
std::string StepString(const ExprProgram& program, uint32_t i) {
  using ExprOp = ExprStep::Op;
  const ExprStep& step = program.steps[i];
  auto binary = [&](const char* op) {
    return "(" + StepString(program, step.a) + " " + op + " " +
           StepString(program, step.b) + ")";
  };
  switch (step.op) {
    case ExprOp::kColumn:
      return "#" + std::to_string(step.a);
    case ExprOp::kConstant:
      return step.type == ExprType::kDouble ? std::to_string(step.double_value)
                                            : std::to_string(step.int_value);
    case ExprOp::kNull:
      return "NULL";
    case ExprOp::kToDouble:
      return "double(" + StepString(program, step.a) + ")";
    case ExprOp::kAdd:
      return binary("+");
    case ExprOp::kSub:
      return binary("-");
    case ExprOp::kMul:
      return binary("*");
    case ExprOp::kDiv:
      return binary("/");
    case ExprOp::kMod:
      return binary("%");
  }
  PERFETTO_FATAL("For GCC");
}

}  // namespace

std::string PipelineFormatter::Format(const Pipeline& pipeline) {
  std::string out = SourceString(pipeline.input());
  for (const Pipeline::Step& step : pipeline.steps()) {
    out += " | ";
    if (const auto* transform =
            std::get_if<std::unique_ptr<Transform>>(&step)) {
      out += TransformString(**transform);
    } else {
      out += OperatorString(*std::get<std::unique_ptr<Operator>>(step));
    }
  }
  return out;
}

std::string PipelineFormatter::SourceString(const Source& source) {
  if (source.Is<DataframeScan>()) {
    return "DataframeScan";
  }
  if (source.Is<GraphTree>()) {
    const GraphTreeSpec& spec = static_cast<const GraphTree&>(source).spec_;
    const char* kind = spec.kind == GraphTreeSpec::Kind::kBfs   ? "BFS"
                       : spec.kind == GraphTreeSpec::Kind::kDfs ? "DFS"
                                                                : "DOMINATOR";
    return std::string("GraphTree(") + kind +
           "; nodes: " + Column(spec.node_column) + " keeping " +
           Columns(spec.retained_columns) +
           "; edges: " + Column(spec.source_column) + " -> " +
           Column(spec.target_column) + "; seeds: " + Column(spec.seed_column) +
           ")";
  }
  if (source.Is<IntervalIntersect>()) {
    const auto& isect = static_cast<const IntervalIntersect&>(source);
    std::string out = "IntervalIntersect(";
    for (size_t i = 0; i < isect.operands_.size(); ++i) {
      out += (i == 0 ? "" : "; ") + Lanes(isect.operands_[i].lanes);
    }
    return out + ")";
  }
  if (source.Is<Pipeline>()) {
    return "Pipeline";
  }
  if (source.Is<SortedScan>()) {
    return "SortedScan";
  }
  if (source.Is<IndexScan>()) {
    return "IndexScan";
  }
  return "Source";
}

std::string PipelineFormatter::TransformString(const Transform& transform) {
  if (transform.Is<AssertType>()) {
    const auto& assert = static_cast<const AssertType&>(transform);
    std::string target = assert.type_.Is<Int64>()    ? "Int64"
                         : assert.type_.Is<Double>() ? "Double"
                                                     : "String";
    return "AssertType(" + Column(assert.column_) + " to " + target + ")";
  }
  if (transform.Is<Distinct>()) {
    return "Distinct(" +
           Columns(static_cast<const Distinct&>(transform).columns_) + ")";
  }
  if (transform.Is<Compute>()) {
    const auto& compute = static_cast<const Compute&>(transform);
    std::string out;
    for (const ExprProgram& program : compute.programs_) {
      out +=
          (out.empty() ? "" : ", ") +
          StepString(program, static_cast<uint32_t>(program.steps.size() - 1));
    }
    return "Compute(" + out + ")";
  }
  if (transform.Is<Filter>()) {
    const auto& filter = static_cast<const Filter&>(transform);
    std::string out;
    for (const Filter::Condition& c : filter.conditions_) {
      out += (out.empty() ? "" : ", ") + Column(c.column) + " " + OpName(c.op);
      if (!c.op.IsAnyOf<NullOp>()) {
        out += " ?" + std::to_string(c.value);
      }
    }
    return "Filter(" + out + ")";
  }
  if (transform.Is<NullColumn>()) {
    const auto& null = static_cast<const NullColumn&>(transform);
    return "NullColumn(" + Column(null.column_) + " as " +
           TypeName(null.type_) + ")";
  }
  if (transform.Is<TreeNumberNodes>()) {
    const auto& number = static_cast<const TreeNumberNodes&>(transform);
    return "TreeNumberNodes(" + Column(number.id_column_) + ", " +
           Column(number.parent_column_) + ")";
  }
  if (transform.Is<TreeAccumulateUp>()) {
    const auto& up = static_cast<const TreeAccumulateUp&>(transform);
    return "TreeAccumulateUp(" + Column(up.spec_.node_column) + ", " +
           Column(up.spec_.parent_column) + "; " +
           Aggregates(up.spec_.aggregates) + ")";
  }
  if (transform.Is<TreeAccumulateDown>()) {
    const auto& down = static_cast<const TreeAccumulateDown&>(transform);
    return "TreeAccumulateDown(" + Column(down.spec_.node_column) + ", " +
           Column(down.spec_.parent_column) + "; " +
           Aggregates(down.spec_.aggregates) + ")";
  }
  return "Transform";
}

std::string PipelineFormatter::OperatorString(const Operator& op) {
  if (op.Is<CombineBatches>()) {
    return "CombineBatches";
  }
  if (op.Is<Sort>()) {
    return "Sort(" + Keys(static_cast<const Sort&>(op).spec_.keys) + ")";
  }
  if (op.Is<GroupedSort>()) {
    const GroupedSortSpec& spec = static_cast<const GroupedSort&>(op).spec_;
    std::string out = "GroupedSort(" + Columns(spec.groups);
    if (!spec.keys.empty()) {
      out += "; " + Keys(spec.keys);
    }
    return out + ")";
  }
  if (op.Is<HashAggregate>()) {
    const HashAggregateSpec& spec = static_cast<const HashAggregate&>(op).spec_;
    return "HashAggregate(keys: " + Columns(spec.key_columns) + "; " +
           Aggregates(spec.aggregates) + ")";
  }
  if (op.Is<IntervalFillGaps>()) {
    const IntervalFillGapsSpec& spec =
        static_cast<const IntervalFillGaps&>(op).spec_;
    return "IntervalFillGaps(" + Lanes(spec.input) +
           "; background: " + Lanes(spec.background.lanes) + ")";
  }
  if (op.Is<IntervalFlatten>()) {
    const IntervalFlattenSpec& spec =
        static_cast<const IntervalFlatten&>(op).spec_;
    std::string out = "IntervalFlatten(" + Lanes(spec.lanes);
    if (!spec.aggregates.empty()) {
      out += "; " + Aggregates(spec.aggregates);
    }
    return out + ")";
  }
  if (op.Is<Limit>()) {
    const auto& limit = static_cast<const Limit&>(op);
    std::string out = "Limit(";
    out += limit.limit_ ? std::to_string(*limit.limit_) : "none";
    if (limit.offset_) {
      out += " offset " + std::to_string(limit.offset_);
    }
    return out + ")";
  }
  if (op.Is<TreeChildFirst>()) {
    const auto& order = static_cast<const TreeChildFirst&>(op);
    return "TreeChildFirst(" + Column(order.node_column_) + ", " +
           Column(order.parent_column_) + ")";
  }
  if (op.Is<TreeParentFirst>()) {
    const auto& order = static_cast<const TreeParentFirst&>(op);
    return "TreeParentFirst(" + Column(order.node_column_) + ", " +
           Column(order.parent_column_) + ")";
  }
  return "Operator";
}

}  // namespace perfetto::trace_processor::core::exec
