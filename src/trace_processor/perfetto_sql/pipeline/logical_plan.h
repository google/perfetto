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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_LOGICAL_PLAN_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_LOGICAL_PLAN_H_

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_flatten.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_intersect.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/tree_accumulate.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

// The operator a plan node holds. Passes switch on `operation.index()` with one
// case per operator, using base::variant_index<PlanOperation, T>().
using PlanOperation =
    std::variant<Scan, TreeAccumulate, IntervalIntersect, IntervalFlatten>;

// An operator together with the relations it reads. A Scan reads none; a
// single-input stage reads one; an intersection reads one per operand.
class PlanNode {
 public:
  PlanNode(PlanOperation operation, std::vector<PlanNodeId> children)
      : operation_(std::move(operation)), children_(std::move(children)) {}

  template <typename T>
  bool Is() const {
    return std::holds_alternative<T>(operation_);
  }

  // Returns the operator as a T. The node must hold one.
  template <typename T>
  T& Cast() {
    return base::unchecked_get<T>(operation_);
  }
  template <typename T>
  const T& Cast() const {
    return base::unchecked_get<T>(operation_);
  }

  std::vector<PlanNodeId>& children() { return children_; }
  const std::vector<PlanNodeId>& children() const { return children_; }

  PlanOperation& operation() { return operation_; }
  const PlanOperation& operation() const { return operation_; }

 private:
  PlanOperation operation_;
  std::vector<PlanNodeId> children_;
};

// -----------------------------------------------------------------------------
// Logical plan
// -----------------------------------------------------------------------------

// A tree of operators. Column types are stored once, indexed by ID; operators
// name the values they consume and produce, independently of layout.
//
// The root is the last stage of the pipeline and the leaves are its sources.
class LogicalPlan {
 public:
  ColumnId AddColumn(std::string name, std::optional<core::StorageType> type) {
    auto id = static_cast<ColumnId>(columns_.size());
    columns_.push_back({std::move(name), type});
    return id;
  }

  // Adds a node reading `children` and makes it the root, which holds while a
  // plan is built bottom up: each node added is the topmost one so far.
  PlanNodeId AddNode(PlanOperation operation,
                     std::vector<PlanNodeId> children = {}) {
    auto id = static_cast<PlanNodeId>(nodes_.size());
    nodes_.push_back({std::move(operation), std::move(children)});
    root_ = id;
    return id;
  }

  std::vector<ColumnSchema>& columns() { return columns_; }
  const std::vector<ColumnSchema>& columns() const { return columns_; }

  std::vector<PlanNode>& nodes() { return nodes_; }
  const std::vector<PlanNode>& nodes() const { return nodes_; }

  PlanNodeId root() const { return root_; }
  void SetRoot(PlanNodeId root) { root_ = root; }

  std::vector<NamedColumn>& output() { return output_; }
  const std::vector<NamedColumn>& output() const { return output_; }

 private:
  // Defining SQL names are stable diagnostic labels, independent of aliases
  // in output bindings. Physical temporaries do not get SQL names or IDs.
  std::vector<ColumnSchema> columns_;
  std::vector<PlanNode> nodes_;
  // The node whose rows are the plan's rows. Pruning can leave unreachable
  // nodes in the vector so that node IDs remain stable.
  PlanNodeId root_ = 0;
  // Visible result columns, in order. Internal columns have no binding here.
  std::vector<NamedColumn> output_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_LOGICAL_PLAN_H_
