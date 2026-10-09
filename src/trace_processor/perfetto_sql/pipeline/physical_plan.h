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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PHYSICAL_PLAN_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PHYSICAL_PLAN_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"

namespace perfetto::trace_processor::pipeline {

// -----------------------------------------------------------------------------
// Execution plan
// -----------------------------------------------------------------------------

// A pipeline ready to run: the executor nodes plus which batch columns are
// the output. Const once built, so it can be run any number of times with one
// state per run.
class PhysicalPlan {
 public:
  struct Column {
    std::string name;
    // Index in a batch of source().
    uint32_t index;
  };

  PhysicalPlan();
  PhysicalPlan(const PhysicalPlan&) = delete;
  PhysicalPlan& operator=(const PhysicalPlan&) = delete;
  ~PhysicalPlan();

  const core::exec::Source& source() const { return *pipeline_; }
  const std::vector<Column>& columns() const { return columns_; }

 private:
  friend class Lowering;

  // Declared first so the input outlives the pipeline that reads it, as an
  // intersection's operands outlive the intersection.
  std::vector<std::unique_ptr<core::exec::Source>> operand_inputs_;
  std::vector<std::unique_ptr<core::exec::Pipeline>> operand_pipelines_;
  std::unique_ptr<core::exec::Source> input_;
  std::unique_ptr<core::exec::Pipeline> pipeline_;
  std::vector<Column> columns_;
};

// Builds executor nodes from a logical plan. Establishes tree numbering, row
// ordering, and column types as needed, reusing them across consecutive folds.
// Every dataframe the plan reads must already be resolved.
base::StatusOr<std::unique_ptr<PhysicalPlan>> Lower(const LogicalPlan&);

// -----------------------------------------------------------------------------
// Lowering
// -----------------------------------------------------------------------------

// Interface used by operation implementations to build an execution pipeline.
// Owns the execution graph, column layout and guarantees established so far.
// The logical plan is borrowed and must outlive this builder. Each operation
// lowers its own children before adding execution operators; intersection
// instead builds independent input pipelines and attaches them with AddOperand.
// ColumnId names a logical value; Position returns its current batch slot.
class Lowering {
 public:
  // Numbered tree columns are physical temporaries, with no logical IDs.
  struct TreeColumns {
    uint32_t node;
    uint32_t parent;
  };

  // The known order of the rows: grouped by `grouped_by` (numbered in
  // `group_column`), with `ascending` ascending within each group.
  struct Order {
    std::vector<ColumnId> grouped_by;
    uint32_t group_column = 0;
    std::vector<ColumnId> ascending;
  };

  // ---------------------------------------------------------------------------
  // Construction and traversal
  // ---------------------------------------------------------------------------

  explicit Lowering(const LogicalPlan& plan);
  // Dispatches to the operation, which is responsible for lowering its inputs.
  void LowerNode(PlanNodeId);
  // Returns the finished plan, or the error that stopped lowering. Call once,
  // after lowering the root.
  base::StatusOr<std::unique_ptr<PhysicalPlan>> Finish();
  // Records an error that stops lowering. Only the first error is kept.
  void Fail(base::Status);

  // ---------------------------------------------------------------------------
  // Execution graph
  // ---------------------------------------------------------------------------

  // Appends execution work. Layout and order metadata are updated separately.
  void AddOperator(core::exec::Pipeline::Step);
  // Owns a separate input pipeline. The returned source lives with the result.
  const core::exec::Source* AddOperand(
      std::unique_ptr<core::exec::Source> source,
      std::vector<core::exec::Pipeline::Step> steps);

  // ---------------------------------------------------------------------------
  // Row ordering and tree preparation
  // ---------------------------------------------------------------------------

  // Groups rows by `keys`, ordered by `ascending` within each group, adding
  // only the Sort and GroupBy not already established.
  void PrepareGroups(const std::vector<ColumnId>& keys, ColumnId ascending);
  // Sorts rows by `keys`, the first deciding, unless they are known to be in
  // that order already. A key of unknown type must hold integers.
  void SortRows(const std::vector<SortKey>& keys);
  // Adds numbering and traversal operators as needed, reusing the current
  // tree until ResetLayout. Returned positions name physical temporaries.
  TreeColumns PrepareTree(ColumnId node, ColumnId parent, TreeDirection);

  // ---------------------------------------------------------------------------
  // Column types
  // ---------------------------------------------------------------------------

  // A column in one input's pipeline.
  struct Side {
    ColumnId column;

    // The column's position in that pipeline's batches.
    uint32_t position;

    // The pipeline's steps, where conversions are added.
    std::vector<core::exec::Pipeline::Step>* steps;
  };
  // Converts every side to the type they all convert to without loss, and
  // returns it. Fails if some sides are strings and others numbers.
  PlanType Unify(const std::vector<Side>&);
  // Like Require below, for a column in an input with its own pipeline.
  void Require(const Side& side, core::StorageType type);

  // Converts `column` to `type`, failing lowering if it cannot be converted
  // without loss.
  void Require(ColumnId column, core::StorageType type);

  // ---------------------------------------------------------------------------
  // Batch column layout
  // ---------------------------------------------------------------------------

  // Assigns the next batch slot to a logical value; does not emit an operator.
  void Define(ColumnId id);
  // Reserves a batch slot with no logical ID.
  uint32_t AllocateTemporaryColumn();
  // Restarts slot allocation and clears ordering/tree guarantees. The caller
  // must Define all surviving values in their new positions before using them.
  void ResetLayout();

  // ---------------------------------------------------------------------------
  // Setters and getters
  // ---------------------------------------------------------------------------

  // Takes ownership of the main input; it must not already be set.
  void SetSource(std::unique_ptr<core::exec::Source> source);
  // Records guarantees of emitted operators; these methods do not sort rows.
  void AddAscendingColumn(ColumnId column);
  void SetOrder(Order order);

  const LogicalPlan& plan() const { return plan_; }
  const Order& order() const { return order_; }
  bool has_source() const;
  // Looks up a live logical value previously registered by Define.
  uint32_t Position(ColumnId id) const;

 private:
  // The type of `column`, after any conversion.
  PlanType TypeOf(ColumnId column) const;
  // Converts `side` to `type`. The conversion must be lossless.
  void Convert(const Side& side, const PlanType& type);

  // ---------------------------------------------------------------------------
  // Shared layout and ordering state
  // ---------------------------------------------------------------------------

  // Borrowed for the duration of lowering.
  const LogicalPlan& plan_;

  // Width of the current batch, including temporary columns without IDs.
  uint32_t column_count_ = 0;

  // Valid while operations preserve the tree. Flattening clears both; sorting
  // for tree traversal replaces order_ but retains the numbered columns.
  std::optional<TreeColumns> tree_columns_;
  std::optional<TreeDirection> tree_direction_;

  // Describes the current rows, not the input plan. Operations which reorder
  // or replace rows must update this to the guarantees their output provides.
  Order order_;

  // ---------------------------------------------------------------------------
  // Owned execution graph and column mappings
  // ---------------------------------------------------------------------------

  // Sources must outlive the execution operators which read them.
  std::unique_ptr<PhysicalPlan> out_;
  std::vector<core::exec::Pipeline::Step> operators_;

  // Only live logical columns have a meaningful physical position.
  std::vector<uint32_t> positions_;

  // For each logical column, the type it was converted to, if any.
  std::vector<std::optional<core::StorageType>> converted_;

  base::Status status_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PHYSICAL_PLAN_H_
