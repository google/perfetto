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
  friend class PlanLowering;

  // Declared first so they are destroyed last: each input must outlive the
  // pipelines that read it.
  std::vector<std::unique_ptr<core::exec::Source>> side_inputs_;
  std::vector<std::unique_ptr<core::exec::Pipeline>> side_pipelines_;
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

class PlanLowering;

// Builds one pipeline. It is either the main pipeline or a side pipeline. A
// side pipeline is an extra input that an operator reads, such as an
// intersection operand. Side pipelines can have side pipelines of their own.
//
// Each operation lowers its children first. Its main input goes into this
// pipeline, and any other inputs go into side pipelines.
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

  // A column and the pipeline it is in.
  struct Side {
    Lowering* pipeline;
    ColumnId column;
  };

  explicit Lowering(PlanLowering* plan);
  Lowering(const Lowering&) = delete;
  Lowering& operator=(const Lowering&) = delete;
  ~Lowering();

  // ---------------------------------------------------------------------------
  // Construction and traversal
  // ---------------------------------------------------------------------------

  // Dispatches to the operation, which is responsible for lowering its inputs.
  void LowerNode(PlanNodeId);

  // Records an error that stops lowering. Only the first error is kept.
  void Fail(base::Status);

  // ---------------------------------------------------------------------------
  // Execution graph
  // ---------------------------------------------------------------------------

  // Takes ownership of the pipeline's input; it must not already be set.
  void SetSource(std::unique_ptr<core::exec::Source> source);

  // Appends execution work. Layout and order metadata are updated separately.
  void AddOperator(core::exec::Pipeline::Step);

  // Creates a new side pipeline.
  Lowering& AddSidePipeline();

  // Finishes this side pipeline and returns the source an operator reads it
  // from. Nothing can be added to the pipeline afterwards.
  const core::exec::Source* FinishSide();

  // ---------------------------------------------------------------------------
  // Row order
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

  // Records that `column` ascends within each group after an emitted
  // operator.
  void AddAscendingColumn(ColumnId column);

  // Replaces what is known of the order with what an emitted operator leaves.
  void SetOrder(Order order);

  // ---------------------------------------------------------------------------
  // Column types
  // ---------------------------------------------------------------------------

  // Converts every side to the type they all convert to without loss, and
  // returns it. Fails if some sides are strings and others numbers.
  PlanType Unify(const std::vector<Side>&);

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

  // Looks up a live logical value previously registered by Define.
  uint32_t Position(ColumnId id) const;

  // ---------------------------------------------------------------------------
  // Getters
  // ---------------------------------------------------------------------------

  const LogicalPlan& plan() const;
  const Order& order() const { return order_; }
  bool has_source() const { return input_ != nullptr; }

 private:
  friend class PlanLowering;

  // Converts `side` to `type`. The conversion must be lossless.
  static void Convert(const Side& side, const PlanType& type);

  // Builds the pipeline from the input and operators added so far.
  std::unique_ptr<core::exec::Pipeline> Build();

  // Borrowed for the duration of lowering.
  PlanLowering* plan_;

  // ---------------------------------------------------------------------------
  // Layout and order of this pipeline
  // ---------------------------------------------------------------------------

  // Width of the current batch, including temporary columns without IDs.
  uint32_t column_count_ = 0;

  // Only live logical columns have a meaningful physical position.
  std::vector<uint32_t> positions_;

  // Valid while operations preserve the tree. Flattening clears both; sorting
  // for tree traversal replaces order_ but retains the numbered columns.
  std::optional<TreeColumns> tree_columns_;
  std::optional<TreeDirection> tree_direction_;

  // Describes the current rows, not the input plan. Operations which reorder
  // or replace rows must update this to the guarantees their output provides.
  Order order_;

  // ---------------------------------------------------------------------------
  // Execution graph of this pipeline
  // ---------------------------------------------------------------------------

  // The pipeline's input.
  std::unique_ptr<core::exec::Source> input_;

  // The pipeline's operators, in the order they run.
  std::vector<core::exec::Pipeline::Step> operators_;
};

// State shared by all pipelines while lowering a plan.
class PlanLowering {
 public:
  explicit PlanLowering(const LogicalPlan& plan);
  PlanLowering(const PlanLowering&) = delete;
  PlanLowering& operator=(const PlanLowering&) = delete;
  ~PlanLowering();

  // The pipeline whose output is the plan's output.
  Lowering& main() { return *pipelines_.front(); }

  // Records an error that stops lowering. Only the first error is kept.
  void Fail(base::Status);

  // Returns the finished plan, or the error that stopped lowering. Call once,
  // after lowering the root into main().
  base::StatusOr<std::unique_ptr<PhysicalPlan>> Finish();

  const LogicalPlan& plan() const { return plan_; }

 private:
  friend class Lowering;

  // The type of `column`, after any conversion.
  PlanType TypeOf(ColumnId column) const;

  // Borrowed for the duration of lowering.
  const LogicalPlan& plan_;

  // For each logical column, the type it was converted to, if any. Kept here
  // because column ids are unique across pipelines.
  std::vector<std::optional<core::StorageType>> converted_;

  // The first error recorded by Fail.
  base::Status status_;

  // The plan being built. Its sources must outlive the operators that read
  // them.
  std::unique_ptr<PhysicalPlan> out_;

  // The main pipeline first, then every side pipeline.
  std::vector<std::unique_ptr<Lowering>> pipelines_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PHYSICAL_PLAN_H_
