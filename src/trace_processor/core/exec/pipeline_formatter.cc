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

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/aggregate_function.h"
#include "src/trace_processor/core/exec/assert_type.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/distinct.h"
#include "src/trace_processor/core/exec/grouped_sort.h"
#include "src/trace_processor/core/exec/hash_aggregate.h"
#include "src/trace_processor/core/exec/interval_flatten.h"
#include "src/trace_processor/core/exec/interval_intersect.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/sort.h"
#include "src/trace_processor/core/exec/sorted_rows.h"
#include "src/trace_processor/core/exec/tree_accumulate.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/exec/tree_order.h"

namespace perfetto::trace_processor::core::exec {
namespace {

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
  if (source.Is<IntervalIntersect>()) {
    const auto& isect = static_cast<const IntervalIntersect&>(source);
    std::string out = "IntervalIntersect(";
    for (size_t i = 0; i < isect.operands_.size(); ++i) {
      const IntervalIntersectOperand& operand = isect.operands_[i];
      out += (i == 0 ? "" : "; ") + Column(operand.ts_column) + ", " +
             Column(operand.dur_column);
      if (!operand.key_columns.empty()) {
        out += ", keys: " + Columns(operand.key_columns) +
               ", groups: " + Column(operand.group_column);
      }
    }
    return out + ")";
  }
  if (source.Is<Pipeline>()) {
    return "Pipeline";
  }
  return "Source";
}

std::string PipelineFormatter::TransformString(const Transform& transform) {
  if (transform.Is<AssertType>()) {
    const auto& assert = static_cast<const AssertType&>(transform);
    std::string target = assert.target_.Is<Int64>()    ? "Int64"
                         : assert.target_.Is<Double>() ? "Double"
                                                       : "String";
    return "AssertType(" + Column(assert.column_) + " to " + target + ")";
  }
  if (transform.Is<Distinct>()) {
    return "Distinct(" +
           Columns(static_cast<const Distinct&>(transform).columns_) + ")";
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
  if (op.Is<IntervalFlatten>()) {
    const IntervalFlattenSpec& spec =
        static_cast<const IntervalFlatten&>(op).spec_;
    std::string out = "IntervalFlatten(" + Column(spec.ts_column) + ", " +
                      Column(spec.dur_column);
    if (!spec.key_columns.empty()) {
      out += ", keys: " + Columns(spec.key_columns) +
             ", groups: " + Column(spec.group_column);
    }
    if (!spec.aggregates.empty()) {
      out += "; " + Aggregates(spec.aggregates);
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
