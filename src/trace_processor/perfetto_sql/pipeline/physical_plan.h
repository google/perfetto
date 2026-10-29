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
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/core/exec/interval_lanes.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/row_order.h"

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

  // The main pipeline. Its output is the plan's output.
  const core::exec::Pipeline& pipeline() const { return *pipeline_; }

  // The side pipelines, in the order they were built. A side pipeline is
  // built before any pipeline that reads it.
  const std::vector<std::unique_ptr<core::exec::Pipeline>>& side_pipelines()
      const {
    return side_pipelines_;
  }

  // The values a Filter compares columns with, known when the plan was
  // built, e.g. a WHERE literal. Passed to Context::SetFilterValues (see
  // core::exec::RowCursor::Open) before each run.
  const std::vector<core::exec::FilterValue>& filter_values() const {
    return filter_values_;
  }

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
  std::vector<core::exec::FilterValue> filter_values_;
};

// What lowering needs from whoever runs the plan. Everything here must outlive
// every run of the plan.
struct LoweringContext {
  // Holds the strings of the columns a Filter reads.
  const StringPool* pool = nullptr;
};

// Builds executor nodes from a logical plan. Establishes tree numbering, row
// ordering, and column types as needed, reusing them across consecutive folds.
// Every dataframe the plan reads must already be resolved.
//
// First rewrites the plan with what binding resolved (see
// PlanOperation::RewriteBound), so `plan` is taken by value.
base::StatusOr<std::unique_ptr<PhysicalPlan>> Lower(LogicalPlan plan,
                                                    const LoweringContext&);

// What an operation rewriting a bound plan can see and change.
class BoundRewriter {
 public:
  BoundRewriter(LogicalPlan* plan, const LoweringContext& context)
      : plan_(plan), context_(context) {}

  LogicalPlan& plan() { return *plan_; }
  const LoweringContext& context() const { return context_; }

  // The order of the rows `node` outputs, as lowering it would leave them.
  // nullopt if it cannot be lowered.
  std::optional<RowOrder> QueryRowOrder(PlanNodeId node) const;

 private:
  LogicalPlan* plan_;
  const LoweringContext& context_;
};

// -----------------------------------------------------------------------------
// Lowering
// -----------------------------------------------------------------------------

// Returns the type that both `a` and `b` convert to without loss. Returns
// nullopt if one is a string and the other a number.
//
// A column of unknown type is checked at runtime to hold the other side's
// type. For integers the check uses Int64, so it accepts any integer.
std::optional<PlanType> CommonType(const PlanType& a, const PlanType& b);

class PlanLowering;

// Builds one pipeline. It is either the main pipeline or a side pipeline. A
// side pipeline is an extra input that an operator reads, such as an
// intersection operand. Side pipelines can have side pipelines of their own.
//
// Each operation lowers its children first. Its main input goes into this
// pipeline, and any other inputs go into side pipelines.
//
// Operations use three kinds of method:
//  - Require...: makes the rows or columns meet a condition. Adds operators
//    only if needed.
//  - Declare...: records what an added operator produces. Adds no operators.
//  - Query...: returns what is currently known. Changes nothing.
//
// A ColumnId identifies a logical column. QueryPosition returns its position
// in the current batch.
class Lowering : private OrderSteps {
 public:
  // Numbered tree columns are physical temporaries, with no logical IDs.
  struct TreeColumns {
    uint32_t node;
    uint32_t parent;
  };

  // A column and the pipeline it is in.
  struct Side {
    Lowering* pipeline;
    ColumnId column;
  };

  explicit Lowering(PlanLowering* plan);
  Lowering(const Lowering&) = delete;
  Lowering& operator=(const Lowering&) = delete;
  ~Lowering() override;

  // ---------------------------------------------------------------------------
  // Construction
  // ---------------------------------------------------------------------------

  // Lowers a plan node. The node's operation lowers its own inputs.
  void LowerNode(PlanNodeId);

  // Records an error that stops lowering. Only the first error is kept.
  void Fail(base::Status);

  // ---------------------------------------------------------------------------
  // Execution graph
  // ---------------------------------------------------------------------------

  // Sets the pipeline's input. It must not already be set.
  void SetSource(std::unique_ptr<core::exec::Source> source);

  // Adds an operator that may depend on the order of the rows. Its output
  // must be declared with the Declare methods.
  void AddOperator(core::exec::Pipeline::Step);

  // Adds `step`, which does not depend on or change the order of rows or the
  // size of batches.
  void AddOrderIndependent(core::exec::Pipeline::Step step);

  // Creates a new side pipeline.
  Lowering& AddSidePipeline();

  // Finishes this side pipeline and returns the source an operator reads it
  // from. Nothing can be added to the pipeline afterwards.
  const core::exec::Source* FinishSide();

  // Adds a filter value known when the plan was built, e.g. a WHERE literal,
  // and returns its index into the run's filter values (see
  // PhysicalPlan::filter_values).
  uint32_t AddFilterValue(core::exec::FilterValue value);

  const LoweringContext& context() const;

  // ---------------------------------------------------------------------------
  // Requirements: add operators only if needed
  // ---------------------------------------------------------------------------

  // Converts `column` to `type`. Fails lowering if the conversion would lose
  // information.
  void RequireType(ColumnId column, core::StorageType type);

  // Converts every side to the type they all convert to without loss, and
  // returns it. Fails if some sides are strings and others numbers.
  PlanType RequireCommonType(const std::vector<Side>&);

  // Sorts rows by `keys`. A key of unknown type must hold integers.
  void RequireSorted(const std::vector<SortKey>& keys);

  // Groups rows by `keys`, so rows with equal keys are next to each other.
  // Groups, and rows within a group, can be in any order. Without keys, all
  // rows are one group.
  void RequireGrouped(const std::vector<ColumnId>& keys);

  // Groups rows by `keys` and sorts each group by `sorted_by`. Without keys,
  // this sorts all rows.
  void RequireSortedWithinGroups(const std::vector<ColumnId>& keys,
                                 const std::vector<SortKey>& sorted_by);

  // Groups rows by `keys` and returns the position of a column holding each
  // row's group number.
  uint32_t RequireGroupNumbers(const std::vector<ColumnId>& keys);

  // Prepares the rows as an interval input: Int64 `ts` and `dur`, grouped by
  // `keys` with group numbers, and sorted by `ts` within each group. Returns
  // where the operator finds them.
  core::exec::IntervalLaneSpec RequireIntervalLanes(
      ColumnId ts,
      ColumnId dur,
      const std::vector<ColumnId>& keys);

  // Use before an operator that works best on full batches. Adds a
  // CombineBatches only if an earlier step may have made small batches.
  void RequireFullBatches();

  // Orders rows as a tree walk in `direction`, and returns the positions of
  // the node number columns. The numbering is reused while `node` and
  // `parent` are the columns it was made from, until ResetLayout.
  TreeColumns RequireTreeWalk(ColumnId node, ColumnId parent, TreeDirection);

  // ---------------------------------------------------------------------------
  // Declarations: record what an added operator produces
  // ---------------------------------------------------------------------------

  // Assigns the next batch column to a logical column.
  void DeclareColumn(ColumnId id);

  // Reserves the next batch column for a value with no logical column, and
  // returns its position.
  uint32_t DeclareTemporaryColumn();

  // Records that an added operator gave `id` values of `type`, for a column
  // whose type was not known when the plan was built.
  void DeclareType(ColumnId id, core::StorageType type);

  // Records that rows are sorted by `keys`.
  void DeclareSorted(const std::vector<SortKey>& keys);

  // Records that rows are grouped by `keys`.
  void DeclareGrouped(const std::vector<ColumnId>& keys);

  // Records that rows are grouped by `keys` and each group is sorted by
  // `sorted_by`.
  void DeclareSortedWithinGroups(const std::vector<ColumnId>& keys,
                                 const std::vector<SortKey>& sorted_by);

  // Records that an added operator may output small batches, for example
  // because it drops rows. Batches are assumed to be full otherwise.
  void DeclareSmallBatches();

  // Records that an added operator drops rows. A tree numbered before may
  // now name parents which are gone, so the next tree walk numbers it again.
  void DeclareRowsDropped();

  // Records that the column at `position` holds group numbers for `keys`.
  void DeclareGroupNumbers(const std::vector<ColumnId>& keys,
                           uint32_t position);

  // Clears all declarations, for an operator that outputs new rows. Every
  // column still used must be declared again.
  void ResetLayout();

  // Records that the rows are a tree walk in `direction`, numbered in the
  // temporary `columns` from `node` and `parent`, as RequireTreeWalk would
  // leave them.
  void DeclareTreeWalk(ColumnId node,
                       ColumnId parent,
                       TreeColumns columns,
                       TreeDirection direction);

  // ---------------------------------------------------------------------------
  // Queries: return what is known, change nothing
  // ---------------------------------------------------------------------------

  // The batch position of a column declared with DeclareColumn.
  uint32_t QueryPosition(ColumnId id) const;

  // The type of a column, after any conversion added so far.
  PlanType QueryType(ColumnId id) const;

  // The longest prefix of the known sort order that uses only `columns`.
  std::vector<SortKey> QuerySortedBy(
      const std::vector<ColumnId>& columns) const;

  // Everything known about the order of the rows.
  const RowOrder& QueryOrder() const { return order_; }

  // ---------------------------------------------------------------------------
  // Accessors
  // ---------------------------------------------------------------------------

  const LogicalPlan& plan() const;
  bool has_source() const { return input_ != nullptr; }

 private:
  friend class PlanLowering;

  // OrderSteps implementation, adding the steps `order_` decides on.
  void MakeComparable(const std::vector<SortKey>& keys) override;
  void Sort(const std::vector<SortKey>& keys) override;
  void RemoveSort() override;
  void GroupedSort(const std::vector<ColumnId>& keys,
                   const std::vector<SortKey>& sorted_by) override;
  void NumberTree(ColumnId node, ColumnId parent) override;
  void WalkTree(TreeDirection direction) override;
  void CombineBatches() override;

  // Converts `side` to `type`. The conversion must be lossless.
  static void Convert(const Side& side, const PlanType& type);

  // Builds the pipeline from the input and operators added so far. `outputs`
  // are the columns read after the pipeline, or null if any may be.
  std::unique_ptr<core::exec::Pipeline> Build(
      const std::vector<uint32_t>* outputs);

  // Borrowed for the duration of lowering.
  PlanLowering* plan_;

  // ---------------------------------------------------------------------------
  // Layout and order of this pipeline
  // ---------------------------------------------------------------------------

  // Number of columns in the current batch, including temporary columns.
  uint32_t column_count_ = 0;

  // The batch position of each logical column. Only valid for declared
  // columns.
  std::vector<uint32_t> positions_;

  // What is known about the order of the rows.
  RowOrder order_;

  // Where the last Sort is in `operators_`, while `order_` can remove it.
  size_t open_sort_index_ = 0;

  // The positions of the tree node number columns, while `order_` has a
  // numbered tree.
  std::optional<TreeColumns> tree_columns_;

  // The position of each set of group numbers `order_` knows of, in order.
  std::vector<uint32_t> group_number_positions_;

  // ---------------------------------------------------------------------------
  // Execution graph of this pipeline
  // ---------------------------------------------------------------------------

  // The pipeline's input, and how many columns it produces.
  std::unique_ptr<core::exec::Source> input_;
  uint32_t source_columns_ = 0;

  // The pipeline's operators, in the order they run.
  std::vector<core::exec::Pipeline::Step> operators_;
};

// State shared by all pipelines while lowering a plan.
class PlanLowering {
 public:
  PlanLowering(const LogicalPlan& plan, const LoweringContext& context);
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

  // The first error recorded by Fail, or ok.
  const base::Status& status() const { return status_; }

 private:
  friend class Lowering;

  // The type of `column`, after any conversion.
  PlanType TypeOf(ColumnId column) const;

  // Borrowed for the duration of lowering.
  const LogicalPlan& plan_;
  LoweringContext context_;

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
