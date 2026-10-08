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
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/perfetto_sql/pipeline/operation_registry.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor {
class StringPool;
namespace core::dataframe {
class Dataframe;
}  // namespace core::dataframe
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::pipeline {

class Catalog;
class Lowering;
class PlanReader;
class PlanWriter;
class LogicalPlan;
class PlanNode;

// -----------------------------------------------------------------------------
// Operation interface
// -----------------------------------------------------------------------------

// One operation in a logical plan. Lowering builds the batch-processing
// core::exec operators which execute it.
class PlanOperation {
 public:
  PlanOperation() = default;
  virtual ~PlanOperation();

  // Copies mutable payload state so pruning or binding a copied plan cannot
  // modify its original. Immutable dataframe columns may remain shared.
  virtual std::unique_ptr<PlanOperation> Clone() const = 0;

  // Updates this payload and marks required input ColumnIds in needed.
  // Returns a child slot to replace this node, or nullopt to keep it. The
  // pruning driver traverses children after this call; implementations do not.
  virtual std::optional<uint32_t> Prune(std::vector<bool>* needed) = 0;

  // Source preparation, each applied to every node by the caller. Operations
  // without source state inherit no-op defaults.
  //
  // Moves SQL into the argument list before encoding, replacing each SQL
  // source with its index in that list.
  virtual void MoveSqlSourcesToDataframeArgs(std::vector<std::string>*);

  // Resolves named dataframes against the catalog after decoding a plan.
  virtual base::Status ResolveDataframes(LogicalPlan*, const Catalog&);

  // Attaches runtime dataframe arguments before lowering the decoded plan.
  virtual base::Status BindDataframeArgs(
      LogicalPlan*,
      const std::vector<const core::dataframe::Dataframe*>&,
      StringPool*);

  // Builds execution steps, including child traversal. Intersection attaches
  // independent operand pipelines; other stages lower their single input.
  virtual void Lower(Lowering*, const PlanNode&) const = 0;

  // Writes this payload and updates the available column IDs. The driver
  // writes outer tags and orders stages; sources encode their own children.
  virtual void Write(PlanWriter*, const PlanNode&, Available*) const = 0;

  virtual const OperationRegistration& registration() const = 0;

 protected:
  // Concrete payloads are copied by Clone; the interface itself is not a value.
  PlanOperation(const PlanOperation&) = default;
  PlanOperation& operator=(const PlanOperation&) = default;
  PlanOperation(PlanOperation&&) noexcept = default;
  PlanOperation& operator=(PlanOperation&&) noexcept = default;
};

// -----------------------------------------------------------------------------
// Plan nodes
// -----------------------------------------------------------------------------

// An operation together with the relations it reads. A Scan reads none; a
// single-input stage reads one; an intersection reads one per operand.
class PlanNode {
 public:
  PlanNode(std::unique_ptr<PlanOperation> operation,
           std::vector<PlanNodeId> children);

  // Plans have value semantics: each copy owns independent mutable payloads.
  PlanNode(const PlanNode&);
  PlanNode& operator=(const PlanNode&);
  PlanNode(PlanNode&&) noexcept;
  PlanNode& operator=(PlanNode&&) noexcept;
  ~PlanNode();

  template <typename T>
  bool Is() const {
    return &operation_->registration() == &T::kRegistration;
  }

  // Returns the operation as a T. The node must hold one.
  template <typename T>
  const T& Cast() const {
    PERFETTO_DCHECK(Is<T>());
    return static_cast<const T&>(*operation_);
  }

  std::vector<PlanNodeId>& children() { return children_; }
  const std::vector<PlanNodeId>& children() const { return children_; }

  PlanOperation& operation() { return *operation_; }
  const PlanOperation& operation() const { return *operation_; }

 private:
  std::vector<PlanNodeId> children_;
  std::unique_ptr<PlanOperation> operation_;
};

// -----------------------------------------------------------------------------
// Logical plan
// -----------------------------------------------------------------------------

// A tree of operations. Column types are stored once, indexed by ID; operations
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
  template <typename T>
  PlanNodeId AddNode(T operation, std::vector<PlanNodeId> children = {}) {
    auto id = static_cast<PlanNodeId>(nodes_.size());
    nodes_.emplace_back(std::make_unique<T>(std::move(operation)),
                        std::move(children));
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
