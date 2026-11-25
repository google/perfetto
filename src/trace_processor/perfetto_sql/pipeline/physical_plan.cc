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

#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/assert_type.h"
#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/combine_batches.h"
#include "src/trace_processor/core/exec/expand.h"
#include "src/trace_processor/core/exec/grouped_sort.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/sink.h"
#include "src/trace_processor/core/exec/sort.h"
#include "src/trace_processor/core/exec/top_k.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/exec/tree_order.h"
#include "src/trace_processor/core/util/arena.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;
namespace {

void Describe(const LoweredStep& step, ex::StepColumns* out) {
  std::visit([out](const auto& s) { s->DescribeColumns(out); }, step);
}

// Whether `step` holds or rebuilds the batches it is given, so is not run on
// them in place.
bool Holds(const LoweredStep& step) {
  return !std::holds_alternative<std::unique_ptr<ex::Transform>>(step);
}

bool Contains(const std::vector<uint32_t>& values, uint32_t target) {
  return std::find(values.begin(), values.end(), target) != values.end();
}

// Puts an Expand in front of each step which needs a `sparse` column of the
// source laid out, as late as it can be: a step reading the column but not
// sparse, or any operator which does not keep sparse columns, as one holds or
// rebuilds its batches. The columns still sparse after the steps are expanded
// for `outputs`, or all of them if it is null, unless `mode` is kLend and
// `outputs` is known.
void PlaceExpands(std::vector<uint32_t> sparse,
                  const std::vector<uint32_t>* outputs,
                  LoweringContext::SparseOutputs mode,
                  core::Arena& arena,
                  std::vector<LoweredStep>* steps) {
  if (sparse.empty()) {
    return;
  }
  std::vector<LoweredStep> placed;
  auto emit_expand = [&](const std::vector<uint32_t>& columns) {
    if (!columns.empty()) {
      placed.emplace_back(
          std::make_unique<ex::Expand>(arena.CopySpan<uint32_t>(columns)));
    }
  };
  for (LoweredStep& step : *steps) {
    if (!sparse.empty()) {
      ex::StepColumns columns;
      Describe(step, &columns);
      if (Holds(step) && !columns.keeps_sparse) {
        emit_expand(sparse);
        sparse.clear();
      } else {
        std::vector<uint32_t> expand;
        std::vector<uint32_t> remaining;
        for (uint32_t column : sparse) {
          if (Contains(columns.reads, column) &&
              !Contains(columns.reads_sparse, column)) {
            expand.push_back(column);
          } else {
            remaining.push_back(column);
          }
        }
        emit_expand(expand);
        sparse = std::move(remaining);
      }
    }
    placed.push_back(std::move(step));
  }
  if (!sparse.empty() &&
      (!outputs || mode != LoweringContext::SparseOutputs::kLend)) {
    std::vector<uint32_t> expand;
    for (uint32_t column : sparse) {
      if (!outputs || Contains(*outputs, column)) {
        expand.push_back(column);
      }
    }
    emit_expand(expand);
  }
  *steps = std::move(placed);
}

// Tells each operator which columns of its input no later step reads, so an
// operator holding rows holds only the others. `steps` follow a source of
// `source_columns` columns. `outputs` are the columns read after them, or
// null if any may be.
void SetUnreadColumns(uint32_t source_columns,
                      const std::vector<uint32_t>* outputs,
                      std::vector<LoweredStep>* steps) {
  // By step, how it uses columns, and how many its input has.
  std::vector<ex::StepColumns> columns(steps->size());
  std::vector<uint32_t> widths = {source_columns};
  for (size_t i = 0; i < steps->size(); ++i) {
    Describe((*steps)[i], &columns[i]);
    widths.push_back(columns[i].keeps == ex::StepColumns::Keeps::kNone
                         ? columns[i].added
                         : widths[i] + columns[i].added);
  }
  // By column of the output of the step being looked at, whether a later
  // step reads it.
  std::vector<bool> read(widths.back(), outputs == nullptr);
  if (outputs) {
    for (uint32_t column : *outputs) {
      read[column] = true;
    }
  }
  for (size_t i = steps->size(); i-- > 0;) {
    const ex::StepColumns& step = columns[i];
    bool keeps_all = step.keeps == ex::StepColumns::Keeps::kAll;
    LoweredStep& held = (*steps)[i];
    // Now by column of the step's input.
    if (keeps_all) {
      read.resize(widths[i]);
    } else {
      read.assign(widths[i], false);
    }
    for (uint32_t column : step.held_reads) {
      read[column] = true;
    }
    if (Holds(held) && keeps_all) {
      std::vector<bool> unread(widths[i]);
      for (uint32_t c = 0; c < widths[i]; ++c) {
        unread[c] = !read[c];
      }
      if (auto* op = std::get_if<std::unique_ptr<ex::Operator>>(&held)) {
        (*op)->SetUnreadColumns(std::move(unread));
      } else {
        std::get<std::unique_ptr<ex::Breaker>>(held)->SetUnreadColumns(
            std::move(unread));
      }
    }
    for (uint32_t column : step.reads) {
      read[column] = true;
    }
  }
}

}  // namespace

std::optional<PlanType> CommonType(const PlanType& a, const PlanType& b) {
  if (a.is_null()) {
    return b;
  }
  if (b.is_null()) {
    return a;
  }
  if (a.is_unknown() && b.is_unknown()) {
    return a;
  }
  if (a.is_unknown() || b.is_unknown()) {
    const PlanType& known = a.is_unknown() ? b : a;
    if (known.storage()->IsAnyOf<core::IntegerType>()) {
      return PlanType(core::Int64{});
    }
    return known;
  }
  core::StorageType x = *a.storage();
  core::StorageType y = *b.storage();
  if (x == y) {
    return a;
  }
  if (x.Is<core::String>() || y.Is<core::String>()) {
    return std::nullopt;
  }
  if (x.Is<core::Double>() || y.Is<core::Double>()) {
    return PlanType(core::Double{});
  }
  return PlanType(core::Int64{});
}

Lowering::Lowering(PlanLowering* plan) : plan_(plan) {}

Lowering::~Lowering() = default;

void Lowering::LowerNode(PlanNodeId id) {
  const PlanNode& node = plan().nodes()[id];
  const OutputProperties& output = plan_->bound_.planned[id].output;
  rows_ = &output;
  node.operation().Lower(this, node);
  rows_ = &output;
  if (input_ && !source_columns_) {
    source_columns_ = static_cast<uint32_t>(output.columns.size());
  }
}

const std::vector<core::exec::ExprProgram>& Lowering::QueryPrograms(
    const PlanNode& node) const {
  return plan_->bound_.planned[plan().IdOf(node)].programs;
}

void Lowering::LowerInput(const PlanNode& node, uint32_t input) {
  LowerNode(node.children()[input]);
  const PlannedInput& planned =
      plan_->bound_.planned[plan().IdOf(node)].inputs[input];
  // Steps only add columns, so every position is already as in the end.
  rows_ = &planned.rows;
  for (const InputStep& step : planned.steps) {
    std::visit([this](const auto& s) { Apply(s); }, step);
  }
}

core::exec::ReadDirection Lowering::QueryScanDirection(
    const PlanNode& node) const {
  return plan_->bound_.planned[plan().IdOf(node)].scan_direction;
}

std::optional<uint32_t> Lowering::QuerySortKeep() const {
  const std::optional<OutputProperties::OpenSort>& sort = rows_->open_sort;
  if (!sort) {
    return std::nullopt;
  }
  return std::get<input_step::Sort>(plan_->bound_.planned[sort->node]
                                        .inputs[sort->input]
                                        .steps[sort->step])
      .keep;
}

void Lowering::Apply(const input_step::Sort& step) {
  std::vector<ex::SortKey> keys;
  for (const SortKey& key : step.keys) {
    keys.push_back({QueryPosition(key.column), key.descending});
  }
  if (step.combine_first) {
    AddOperator(std::make_unique<ex::CombineBatches>());
  }
  if (step.keep) {
    AddOperator(std::make_unique<ex::TopK>(
        ex::TopKSpec{std::move(keys), context().pool, *step.keep}));
    return;
  }
  AddOperator(std::make_unique<ex::Sort>(
      ex::SortSpec{std::move(keys), context().pool}));
}

void Lowering::Apply(const input_step::Group& step) {
  ex::GroupedSortSpec spec;
  for (ColumnId key : step.keys) {
    spec.groups.push_back(QueryPosition(key));
  }
  for (const SortKey& key : step.sorted_by) {
    spec.keys.push_back({QueryPosition(key.column), key.descending});
  }
  if (step.combine_first) {
    AddOperator(std::make_unique<ex::CombineBatches>());
  }
  AddOperator(std::make_unique<ex::GroupedSort>(std::move(spec)));
}

void Lowering::Apply(const input_step::NumberTree& step) {
  AddOperator(std::make_unique<ex::TreeNumberNodes>(
      QueryPosition(step.node), QueryPosition(step.parent)));
}

void Lowering::Apply(const input_step::WalkTree& step) {
  if (step.combine_first) {
    AddOperator(std::make_unique<ex::CombineBatches>());
  }
  uint32_t node = QueryPosition(step.node_number);
  uint32_t parent = QueryPosition(step.parent_number);
  if (step.direction == TreeDirection::kUp) {
    AddOperator(std::make_unique<ex::TreeChildFirst>(node, parent));
  } else {
    AddOperator(std::make_unique<ex::TreeParentFirst>(node, parent));
  }
}

void Lowering::Apply(const input_step::CombineBatches&) {
  AddOperator(std::make_unique<ex::CombineBatches>());
}

void Lowering::Apply(const input_step::Convert& step) {
  uint32_t position = QueryPosition(step.column);
  if (step.all_null) {
    AddOperator(std::make_unique<ex::NullColumn>(position, step.type));
    return;
  }
  ex::AssertTypeTarget target = core::String{};
  if (step.type.Is<core::Int64>()) {
    target = core::Int64{};
  } else if (step.type.Is<core::Double>()) {
    target = core::Double{};
  }
  PERFETTO_DCHECK(target.Is<core::String>() == step.type.Is<core::String>());
  AddOperator(std::make_unique<ex::AssertType>(
      position, target,
      plan().names().text(plan().columns()[step.column].name)));
}

core::exec::IntervalLaneSpec Lowering::QueryIntervalLanes(
    ColumnId ts,
    ColumnId dur,
    const std::vector<ColumnId>& keys) const {
  core::exec::IntervalLaneSpec spec;
  if (!keys.empty()) {
    spec.group_column = QueryPosition(*rows_->GroupNumbersOf(keys));
  }
  spec.ts_column = QueryPosition(ts);
  spec.dur_column = QueryPosition(dur);
  for (ColumnId key : keys) {
    spec.key_columns.push_back(QueryPosition(key));
  }
  return spec;
}

void Lowering::SetSource(std::unique_ptr<core::exec::Source> source) {
  PERFETTO_DCHECK(!input_);
  input_ = std::move(source);
}

void Lowering::SetSparseColumns(std::vector<uint32_t> columns) {
  sparse_columns_ = std::move(columns);
}

void Lowering::AddOperator(std::unique_ptr<ex::Transform> step) {
  operators_.emplace_back(std::move(step));
}

void Lowering::AddOperator(std::unique_ptr<ex::Operator> step) {
  operators_.emplace_back(std::move(step));
}

void Lowering::AddOperator(std::unique_ptr<ex::Breaker> step) {
  operators_.emplace_back(std::move(step));
}

Lowering& Lowering::AddSidePipeline() {
  plan_->lowerings_.push_back(std::make_unique<Lowering>(plan_));
  return *plan_->lowerings_.back();
}

uint32_t Lowering::FinishSide(std::unique_ptr<ex::Sink> sink) {
  Built built = Build(nullptr);
  return plan_->out_->pipelines_.Add(
      std::move(built.input), std::move(built.pipeline), std::move(sink));
}

std::optional<uint32_t> Lowering::TakeParam(const PlanNode& node,
                                            uint32_t condition) {
  PlannedParam& value =
      plan_->bound_.planned[plan().IdOf(node)].params[condition];
  PERFETTO_CHECK(value.state != PlannedParam::State::kUnplanned &&
                 value.state != PlannedParam::State::kTaken);
  if (value.state == PlannedParam::State::kNone) {
    return std::nullopt;
  }
  PhysicalPlan& out = *plan_->out_;
  auto index = static_cast<uint32_t>(out.params_.size());
  out.params_.push_back(std::move(value.value));
  out.param_sources_.push_back(
      {value.state, std::move(value.literals), value.type, value.op});
  value.state = PlannedParam::State::kTaken;
  return index;
}

const LoweringContext& Lowering::context() const {
  return plan_->context_;
}

uint32_t Lowering::QueryPosition(ColumnId id) const {
  std::optional<uint32_t> position = rows_->PositionOf(id);
  PERFETTO_DCHECK(position);
  return *position;
}

const LogicalPlan& Lowering::plan() const {
  return plan_->plan();
}

const Bindings::Dataframe& Lowering::QueryDataframe(
    const PlanNode& node) const {
  const std::optional<Bindings::Dataframe>& dataframe =
      plan_->bound_.bindings.dataframes[plan().IdOf(node)];
  PERFETTO_CHECK(dataframe);
  return *dataframe;
}

core::Arena& Lowering::arena() const {
  return plan_->out_->arena_;
}

core::ArenaSpan<const core::dataframe::Column*> Lowering::KeepColumns(
    const std::vector<std::shared_ptr<const core::dataframe::Column>>&
        columns) {
  auto kept = arena().NewSpan<const core::dataframe::Column*>(
      static_cast<uint32_t>(columns.size()));
  for (uint32_t i = 0; i < columns.size(); ++i) {
    kept[i] = columns[i].get();
    KeepAlive(columns[i]);
  }
  return kept;
}

void Lowering::KeepAlive(std::shared_ptr<const void> object) {
  plan_->out_->kept_alive_.push_back(std::move(object));
}

Lowering::Built Lowering::Build(const std::vector<uint32_t>* outputs) {
  // Chosen first: a sort serving in place keeps sparse columns unexpanded.
  if (outputs && !operators_.empty() &&
      context().sorted_outputs == LoweringContext::SortedOutputs::kInPlace) {
    auto* last = std::get_if<std::unique_ptr<ex::Breaker>>(&operators_.back());
    if (last && (*last)->Is<ex::Sort>()) {
      static_cast<ex::Sort&>(**last).ServeInPlace();
    }
  }
  PlaceExpands(std::move(sparse_columns_), outputs, context().sparse_outputs,
               arena(), &operators_);
  SetUnreadColumns(*source_columns_, outputs, &operators_);
  // Cut at each breaker, which the pipeline so far ends in.
  ex::Pipelines& pipelines = plan_->out_->pipelines_;
  std::unique_ptr<ex::Source> input = std::move(input_);
  std::vector<ex::Pipeline::Step> steps;
  for (LoweredStep& step : operators_) {
    if (auto* t = std::get_if<std::unique_ptr<ex::Transform>>(&step)) {
      steps.emplace_back(std::move(*t));
      continue;
    }
    if (auto* op = std::get_if<std::unique_ptr<ex::Operator>>(&step)) {
      steps.emplace_back(std::move(*op));
      continue;
    }
    auto pipeline = std::make_unique<ex::Pipeline>(*input, std::move(steps));
    steps.clear();
    std::unique_ptr<ex::Breaker> breaker =
        std::move(std::get<std::unique_ptr<ex::Breaker>>(step));
    const ex::Breaker& served = *breaker;
    uint32_t id = pipelines.Add(std::move(input), std::move(pipeline),
                                std::move(breaker));
    input = std::make_unique<ex::Served>(id, served);
  }
  operators_.clear();
  auto pipeline = std::make_unique<ex::Pipeline>(*input, std::move(steps));
  return {std::move(input), std::move(pipeline)};
}

PlanLowering::PlanLowering(BoundPlan* bound, const LoweringContext& context)
    : bound_(*bound),
      context_(context),
      out_(std::make_unique<PhysicalPlan>()) {
  out_->pool_ = context.pool;
  lowerings_.push_back(std::make_unique<Lowering>(this));
}

PlanLowering::~PlanLowering() = default;

std::unique_ptr<PhysicalPlan> PlanLowering::Finish() {
  Lowering& lowering = main();
  std::vector<uint32_t> outputs;
  for (const NamedColumn& column : plan().output()) {
    outputs.push_back(lowering.QueryPosition(column.id));
    out_->columns_.push_back(
        {plan().names().text(column.name), outputs.back()});
  }
  Lowering::Built built = lowering.Build(&outputs);
  out_->input_ = std::move(built.input);
  out_->pipeline_ = std::move(built.pipeline);
  return std::move(out_);
}

PhysicalPlan::PhysicalPlan() = default;
PhysicalPlan::~PhysicalPlan() = default;

std::unique_ptr<PhysicalPlan> Lower(BoundPlan* bound,
                                    const LoweringContext& context) {
  PlanLowering lowering(bound, context);
  lowering.main().LowerNode(bound->plan.root());
  return lowering.Finish();
}

}  // namespace perfetto::trace_processor::pipeline
