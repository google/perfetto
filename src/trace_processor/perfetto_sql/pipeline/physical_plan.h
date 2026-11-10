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
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/core/exec/interval_lanes.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/output_properties.h"

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
  // Holds the strings of the columns read, and takes the strings of literals
  // which expressions output.
  StringPool* pool = nullptr;
};

// Builds executor nodes from a bound plan. Establishes tree numbering, row
// ordering, and column types as needed, reusing them across consecutive folds.
// Every dataframe the plan reads must be bound.
std::unique_ptr<PhysicalPlan> Lower(BoundPlan*, const LoweringContext&);

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
// Operations only add their own operators: what the rows need is planned before
// lowering (see PlanOperation::Needs), and lowering an input runs it.
//
// A ColumnId identifies a logical column. QueryPosition returns its position
// in the current batch.
class Lowering {
 public:
  explicit Lowering(PlanLowering* plan);
  Lowering(const Lowering&) = delete;
  Lowering& operator=(const Lowering&) = delete;
  ~Lowering();

  // ---------------------------------------------------------------------------
  // Construction
  // ---------------------------------------------------------------------------

  // Lowers input `input` of `node` into this pipeline, leaving its rows as the
  // node needs them.
  void LowerInput(const PlanNode& node, uint32_t input);

  // Lowers a plan node. The node's operation lowers its own inputs.
  void LowerNode(PlanNodeId);

  // ---------------------------------------------------------------------------
  // Execution graph
  // ---------------------------------------------------------------------------

  // Sets the pipeline's input. It must not already be set.
  void SetSource(std::unique_ptr<core::exec::Source> source);

  // Adds an operator.
  void AddOperator(core::exec::Pipeline::Step);

  // Creates a new side pipeline.
  Lowering& AddSidePipeline();

  // Finishes this side pipeline and returns the source an operator reads it
  // from. Nothing can be added to the pipeline afterwards.
  const core::exec::Source* FinishSide();

  // Moves the value condition `condition` of `node` tests with into the plan,
  // if it has one, and returns its index into the run's filter values (see
  // PhysicalPlan::filter_values).
  std::optional<uint32_t> TakeFilterValue(const PlanNode& node,
                                          uint32_t condition);

  const LoweringContext& context() const;

  // ---------------------------------------------------------------------------
  // Queries: return what is known, change nothing
  // ---------------------------------------------------------------------------

  // The programs of the expressions `node` computes.
  const std::vector<core::exec::ExprProgram>& QueryPrograms(
      const PlanNode& node) const;

  // What is known of the rows of the input just lowered.
  const OutputProperties& QueryRows() const { return *rows_; }

  // Where an interval operator finds the input just lowered (see
  // IntervalLaneNeeds).
  core::exec::IntervalLaneSpec QueryIntervalLanes(
      ColumnId ts,
      ColumnId dur,
      const std::vector<ColumnId>& keys) const;

  // The batch position of a column.
  uint32_t QueryPosition(ColumnId id) const;

  // ---------------------------------------------------------------------------
  // Accessors
  // ---------------------------------------------------------------------------

  const LogicalPlan& plan() const;

  // The dataframe the scan `node` is bound to.
  const Bindings::Dataframe& QueryDataframe(const PlanNode& node) const;
  bool has_source() const { return input_ != nullptr; }

 private:
  friend class PlanLowering;

  // Adds the operators of a step.
  void Apply(const input_step::Sort&);
  void Apply(const input_step::Group&);
  void Apply(const input_step::NumberTree&);
  void Apply(const input_step::WalkTree&);
  void Apply(const input_step::CombineBatches&);
  void Apply(const input_step::Convert&);

  // Builds the pipeline from the input and operators added so far. `outputs`
  // are the columns read after the pipeline, or null if any may be.
  std::unique_ptr<core::exec::Pipeline> Build(
      const std::vector<uint32_t>* outputs);

  // Borrowed for the duration of lowering.
  PlanLowering* plan_;

  // ---------------------------------------------------------------------------
  // Layout and order of this pipeline
  // ---------------------------------------------------------------------------

  // What is known of the rows at this point in lowering.
  const OutputProperties* rows_ = nullptr;

  // ---------------------------------------------------------------------------
  // Execution graph of this pipeline
  // ---------------------------------------------------------------------------

  // The pipeline's input, and how many columns it produces.
  std::unique_ptr<core::exec::Source> input_;
  std::optional<uint32_t> source_columns_;

  // The pipeline's operators, in the order they run.
  std::vector<core::exec::Pipeline::Step> operators_;
};

// State shared by all pipelines while lowering a plan.
class PlanLowering {
 public:
  PlanLowering(BoundPlan* bound, const LoweringContext& context);
  PlanLowering(const PlanLowering&) = delete;
  PlanLowering& operator=(const PlanLowering&) = delete;
  ~PlanLowering();

  // The pipeline whose output is the plan's output.
  Lowering& main() { return *pipelines_.front(); }

  // Returns the finished plan, or the error that stopped lowering. Call once,
  // after lowering the root into main().
  std::unique_ptr<PhysicalPlan> Finish();

  const LogicalPlan& plan() const { return bound_.plan; }

 private:
  friend class Lowering;

  // Borrowed for the duration of lowering.
  BoundPlan& bound_;
  LoweringContext context_;

  // The plan being built. Its sources must outlive the operators that read
  // them.
  std::unique_ptr<PhysicalPlan> out_;

  // The main pipeline first, then every side pipeline.
  std::vector<std::unique_ptr<Lowering>> pipelines_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PHYSICAL_PLAN_H_
