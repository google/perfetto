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
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/perfetto_sql/pipeline/operation_registry.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor {
class StringPool;
namespace core::dataframe {
class Dataframe;
}  // namespace core::dataframe
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::pipeline {

struct Bindings;
class Catalog;
class Lowering;
class PlanCodec;
class PlanKey;
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
  virtual void MoveSqlSourcesToDataframeArgs(const NameTable&,
                                             std::vector<std::string>*);

  // Binding, which records what it finds in `bindings` for node `self`,
  // holding this operation, and leaves the plan as it is.
  //
  // Resolves named dataframes against the catalog. A plan read back is
  // checked against the stamp `key` gave each when it was written; a plan
  // compiled in this process passes no key.
  virtual base::Status ResolveDataframes(const LogicalPlan&,
                                         PlanNodeId self,
                                         const Catalog&,
                                         const PlanKey* key,
                                         Bindings* bindings) const;

  // Attaches the dataframes a plan is passed when it runs.
  virtual base::Status BindDataframeArgs(
      const LogicalPlan&,
      PlanNodeId self,
      const std::vector<const core::dataframe::Dataframe*>&,
      StringPool*,
      Bindings* bindings) const;

  // Builds execution steps, including child traversal. Intersection attaches
  // independent operand pipelines; other stages lower their single input.
  virtual void Lower(Lowering*, const PlanNode&) const = 0;

  // Writes or reads this payload, as the codec does.
  virtual void Serialize(PlanCodec&) = 0;

  virtual const OperationRegistration& registration() const = 0;

 protected:
  // Concrete payloads are copied by Clone; the interface itself is not a value.
  PlanOperation(const PlanOperation&) = default;
  PlanOperation& operator=(const PlanOperation&) = default;
  PlanOperation(PlanOperation&&) noexcept = default;
  PlanOperation& operator=(PlanOperation&&) noexcept = default;
};

// An empty operation of kind T, for a registration to make.
template <typename T>
std::unique_ptr<PlanOperation> MakeOperation() {
  return std::make_unique<T>();
}

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
  T& Cast() {
    PERFETTO_DCHECK(Is<T>());
    return static_cast<T&>(*operation_);
  }
  template <typename T>
  const T& Cast() const {
    PERFETTO_DCHECK(Is<T>());
    return static_cast<const T&>(*operation_);
  }

  std::vector<PlanNodeId>& children() { return children_; }
  const std::vector<PlanNodeId>& children() const { return children_; }

  PlanOperation& operation() { return *operation_; }
  const PlanOperation& operation() const { return *operation_; }

  // Swaps the operations of this node and `other`, each keeping its children.
  void SwapOperation(PlanNode* other) { operation_.swap(other->operation_); }

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
  ColumnId AddColumn(NameId name, PlanType type) {
    auto id = static_cast<ColumnId>(columns_.size());
    columns_.push_back({name, type});
    return id;
  }

  // Adds a node reading `children` and makes it the root, which holds while a
  // plan is built bottom up: each node added is the topmost one so far.
  template <typename T>
  PlanNodeId AddNode(T operation, std::vector<PlanNodeId> children = {}) {
    std::unique_ptr<PlanOperation> op =
        std::make_unique<T>(std::move(operation));
    return AddNode(std::move(op), std::move(children));
  }
  PlanNodeId AddNode(std::unique_ptr<PlanOperation> operation,
                     std::vector<PlanNodeId> children) {
    auto id = static_cast<PlanNodeId>(nodes_.size());
    nodes_.emplace_back(std::move(operation), std::move(children));
    root_ = id;
    return id;
  }

  std::vector<PlanColumn>& columns() { return columns_; }
  const std::vector<PlanColumn>& columns() const { return columns_; }

  std::vector<PlanNode>& nodes() { return nodes_; }
  const std::vector<PlanNode>& nodes() const { return nodes_; }

  PlanNodeId root() const { return root_; }
  void SetRoot(PlanNodeId root) { root_ = root; }

  // Drops `node` from the plan: whatever read it reads its first input
  // instead. The node stays in nodes(), unread, so IDs do not change.
  void Bypass(PlanNodeId node);

  std::vector<NamedColumn>& output() { return output_; }
  const std::vector<NamedColumn>& output() const { return output_; }

  NameTable& names() { return names_; }
  const NameTable& names() const { return names_; }

  // The ID of `node`, which must be one of nodes().
  PlanNodeId IdOf(const PlanNode& node) const {
    PERFETTO_DCHECK(&node >= nodes_.data() &&
                    &node < nodes_.data() + nodes_.size());
    return static_cast<PlanNodeId>(&node - nodes_.data());
  }

 private:
  // Defining SQL names are stable diagnostic labels, independent of aliases
  // in output bindings. Physical temporaries do not get SQL names or IDs.
  std::vector<PlanColumn> columns_;
  std::vector<PlanNode> nodes_;
  // The node whose rows are the plan's rows. Pruning can leave unreachable
  // nodes in the vector so that node IDs remain stable.
  PlanNodeId root_ = 0;
  // Visible result columns, in order. Internal columns have no binding here.
  std::vector<NamedColumn> output_;
  NameTable names_;
};

// What binding a plan found for it, kept beside the plan so the plan stays as
// it was written: the dataframe each scan reads and the type of each column.
struct Bindings {
  // A dataframe a scan reads.
  struct Dataframe {
    // By scan column, the dataframe column it reads.
    std::vector<std::shared_ptr<const core::dataframe::Column>> columns;
    uint32_t row_count = 0;
    // The dataframe's indexes on columns the scan reads.
    std::vector<core::dataframe::Index> indexes;
  };

  // By node, the dataframe a scan reads. Unset for any other node.
  std::vector<std::optional<Dataframe>> dataframes;

  // By column, its type: the one it was written with, unless a dataframe
  // passed to the plan decides it.
  std::vector<PlanType> types;
};

// A plan and what binding it found.
struct BoundPlan {
  // `plan` with nothing bound yet.
  explicit BoundPlan(LogicalPlan plan);

  LogicalPlan plan;
  Bindings bindings;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_LOGICAL_PLAN_H_
