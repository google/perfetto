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
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/dataframe_windows.h"
#include "src/trace_processor/core/exec/interval_lanes.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/param_value.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/sink.h"
#include "src/trace_processor/core/util/arena.h"
#include "src/trace_processor/perfetto_sql/pipeline/literal.h"
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

  const core::exec::Source& source() const {
    return pipeline_->steps().empty() ? pipeline_->input() : *pipeline_;
  }
  const std::vector<Column>& columns() const { return columns_; }

  // The main pipeline. Its output is the plan's output.
  const core::exec::Pipeline& pipeline() const { return *pipeline_; }

  // The plan's pipelines besides the main one: each pipeline a breaker ends,
  // and each side pipeline (see core::exec::Pipelines). Run the plan with
  // them (see core::exec::RowCursor).
  const core::exec::Pipelines& pipelines() const { return pipelines_; }

  // The plan's constant params: the values its operators compare with,
  // cast when the plan was built from its literals. Passed to
  // Context::SetParams (see core::exec::RowCursor::Open) before each run. A
  // plan with bound params is run with RunParams instead.
  const std::vector<core::exec::ParamValue>& params() const { return params_; }

 private:
  friend class Lowering;
  friend class PlanLowering;
  template <typename Fetcher>
  friend class RunParams;

  struct ParamSource {
    PlannedParam::State state;
    std::vector<Literal> literals;
    core::StorageType type;
    core::Op op;
  };

  // Holds the lists and strings the plan's operators read.
  core::Arena arena_;
  // What the operators read that does need destroying, such as the dataframe
  // columns scans read. The plan holds it so the operators need no destructor.
  std::vector<std::shared_ptr<const void>> kept_alive_;
  // Declared first so they are destroyed last: each input must outlive the
  // pipelines that read it, and a sink the source which serves it.
  core::exec::Pipelines pipelines_;
  std::unique_ptr<core::exec::Source> input_;
  std::unique_ptr<core::exec::Pipeline> pipeline_;
  std::vector<Column> columns_;
  std::vector<core::exec::ParamValue> params_;
  // By param.
  std::vector<ParamSource> param_sources_;
  const StringPool* pool_ = nullptr;
};

// A cursor's params for its runs of a plan. Constant params are cast once,
// external literal `i` on SetLiterals() from value `i` of its `fetcher`, and
// bound param `i` on each Bind() from value `i` of `fetcher`.
template <typename Fetcher>
class RunParams {
 public:
  explicit RunParams(const PhysicalPlan& plan)
      : pool_(plan.pool_), params_(plan.param_sources_.size()) {
    for (uint32_t i = 0; i < params_.size(); ++i) {
      const PhysicalPlan::ParamSource& s = plan.param_sources_[i];
      if (s.state == PlannedParam::State::kBound ||
          s.state == PlannedParam::State::kExternal) {
        Binding binding{s.op.Is<core::In>()
                            ? core::exec::GetParamListCaster<Fetcher>(s.type)
                            : core::exec::GetParamCaster<Fetcher>(s.type, s.op),
                        s.literals[0].bound, i, s.op.Is<core::Glob>()};
        std::vector<Binding>& to =
            s.state == PlannedParam::State::kBound ? bindings_ : literals_;
        to.push_back(binding);
      } else if (!s.literals.empty()) {
        params_[i] = CastLiterals(s.op, s.literals.data(),
                                  static_cast<uint32_t>(s.literals.size()),
                                  s.type, *pool_);
        if (s.op.Is<core::Glob>()) {
          core::exec::AttachGlob(&params_[i]);
        }
      }
    }
  }

  // Casts the external literals from `fetcher`. Called once per execution of
  // the plan, before its first run.
  void SetLiterals(Fetcher& fetcher) {
    for (const Binding& b : literals_) {
      b.cast(fetcher, b.value, *pool_, &params_[b.param]);
      if (b.glob) {
        core::exec::AttachGlob(&params_[b.param]);
      }
    }
  }

  // Casts the bound params from `fetcher`, and returns the run's params.
  base::Span<const core::exec::ParamValue> Bind(Fetcher& fetcher) {
    for (const Binding& b : bindings_) {
      b.cast(fetcher, b.value, *pool_, &params_[b.param]);
    }
    return params_;
  }

 private:
  struct Binding {
    core::exec::ParamCaster<Fetcher> cast;
    uint32_t value;
    uint32_t param;
    // A GLOB literal, matched once however many runs read it.
    bool glob;
  };
  const StringPool* pool_;
  std::vector<core::exec::ParamValue> params_;
  std::vector<Binding> bindings_;
  std::vector<Binding> literals_;
};

// What lowering needs from whoever runs the plan. Everything here must outlive
// every run of the plan.
struct LoweringContext {
  // How the caller reads the plan's output columns which a scan lends sparse
  // (see core::exec::ColumnView::Kind::kSparse).
  enum class SparseOutputs : uint8_t {
    // Expanded before the output, so the caller reads only flat columns.
    kExpand,
    // Left as lent, as the caller reads sparse columns.
    kLend,
  };

  // How the caller reads the rows of a plan ending in a sort.
  enum class SortedOutputs : uint8_t {
    // Copied into batches in sorted order.
    kCopied,
    // Read where the sort holds them (see core::exec::Source::InPlace).
    kInPlace,
  };

  // Holds the strings of the columns read, and takes the strings of literals
  // which expressions output.
  StringPool* pool = nullptr;
  SparseOutputs sparse_outputs = SparseOutputs::kExpand;
  SortedOutputs sorted_outputs = SortedOutputs::kCopied;
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

// A step of a pipeline being lowered. A breaker ends the pipeline it is in,
// as the sink of the steps before it, and the steps after it read what it
// built (see core::exec::Served).
using LoweredStep = std::variant<std::unique_ptr<core::exec::Transform>,
                                 std::unique_ptr<core::exec::Operator>,
                                 std::unique_ptr<core::exec::Breaker>>;

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
  // The columns the source lends sparse (see ColumnView::Kind::kSparse),
  // which Build() expands where they are first needed laid out.
  void SetSparseColumns(std::vector<uint32_t> columns);

  // Adds an operator.
  void AddOperator(std::unique_ptr<core::exec::Transform>);
  void AddOperator(std::unique_ptr<core::exec::Operator>);
  void AddOperator(std::unique_ptr<core::exec::Breaker>);

  // Holds the lists and strings the plan's operators read. It is freed with
  // the plan, without running destructors.
  core::Arena& arena() const;

  // Keeps `columns` alive as long as the plan, and returns plain pointers to
  // them for an operator to read.
  core::ArenaSpan<const core::dataframe::Column*> KeepColumns(
      const std::vector<std::shared_ptr<const core::dataframe::Column>>&
          columns);

  // Keeps `object` alive as long as the plan.
  void KeepAlive(std::shared_ptr<const void> object);

  // Creates a new side pipeline.
  Lowering& AddSidePipeline();

  // Finishes this side pipeline and returns its id. If `sink` isn't null the
  // pipeline ends in it; otherwise its rows are streamed. The step that reads
  // the pipeline uses the id to find it through the run's Context (see
  // core::exec::PipelineUse). Nothing can be added to the pipeline after
  // this.
  uint32_t FinishSide(std::unique_ptr<core::exec::Sink> sink = nullptr);

  // Moves the value condition `condition` of `node` tests with into the plan,
  // if it has one, and returns its index into the run's params (see
  // PhysicalPlan::params).
  std::optional<uint32_t> TakeParam(const PlanNode& node, uint32_t condition);

  const LoweringContext& context() const;

  // ---------------------------------------------------------------------------
  // Queries: return what is known, change nothing
  // ---------------------------------------------------------------------------

  // The programs of the expressions `node` computes.
  const std::vector<core::exec::ExprProgram>& QueryPrograms(
      const PlanNode& node) const;

  // What is known of the rows of the input just lowered.
  const OutputProperties& QueryRows() const { return *rows_; }

  // How many rows the open sort of the input just lowered outputs, if it is
  // limited to its first rows.
  std::optional<uint32_t> QuerySortKeep() const;

  // Which way the scan `node` reads its rows.
  core::exec::ReadDirection QueryScanDirection(const PlanNode& node) const;

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

  // The last of the pipelines Build() cuts at each breaker, and the source it
  // reads.
  struct Built {
    std::unique_ptr<core::exec::Source> input;
    std::unique_ptr<core::exec::Pipeline> pipeline;
  };

  // Builds the pipeline from the input and operators added so far, ending
  // the pipeline so far in each breaker as a sink of the plan. `outputs` are
  // the columns read after the pipeline, or null if any may be.
  Built Build(const std::vector<uint32_t>* outputs);

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
  std::vector<LoweredStep> operators_;
  std::vector<uint32_t> sparse_columns_;
};

// State shared by all pipelines while lowering a plan.
class PlanLowering {
 public:
  PlanLowering(BoundPlan* bound, const LoweringContext& context);
  PlanLowering(const PlanLowering&) = delete;
  PlanLowering& operator=(const PlanLowering&) = delete;
  ~PlanLowering();

  // The pipeline whose output is the plan's output.
  Lowering& main() { return *lowerings_.front(); }

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
  std::vector<std::unique_ptr<Lowering>> lowerings_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PHYSICAL_PLAN_H_
