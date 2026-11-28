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
  return !std::holds_alternative<ex::Transform*>(step);
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
  for (LoweredStep& step : *steps) {
    std::vector<uint32_t> expand;
    ex::StepColumns columns;
    Describe(step, &columns);
    if (Holds(step) && !columns.keeps_sparse) {
      expand = std::move(sparse);
      sparse.clear();
    } else {
      for (auto it = sparse.begin(); it != sparse.end();) {
        bool reads = std::find(columns.reads.begin(), columns.reads.end(),
                               *it) != columns.reads.end();
        bool lent =
            std::find(columns.reads_sparse.begin(), columns.reads_sparse.end(),
                      *it) != columns.reads_sparse.end();
        if (reads && !lent) {
          expand.push_back(*it);
          it = sparse.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (!expand.empty()) {
      placed.emplace_back(
          arena.New<ex::Expand>(arena.CopySpan<uint32_t>(expand)));
    }
    placed.push_back(std::move(step));
  }
  if (outputs && mode == LoweringContext::SparseOutputs::kLend) {
    *steps = std::move(placed);
    return;
  }
  std::vector<uint32_t> expand;
  for (uint32_t column : sparse) {
    if (!outputs ||
        std::find(outputs->begin(), outputs->end(), column) != outputs->end()) {
      expand.push_back(column);
    }
  }
  if (!expand.empty()) {
    placed.emplace_back(
        arena.New<ex::Expand>(arena.CopySpan<uint32_t>(expand)));
  }
  *steps = std::move(placed);
}

// Tells each operator which columns of its input no later step reads, so an
// operator holding rows holds only the others. `steps` follow a source of
// `source_columns` columns. `outputs` are the columns read after them, or
// null if any may be.
void SetUnreadColumns(uint32_t source_columns,
                      const std::vector<uint32_t>* outputs,
                      core::Arena& arena,
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
      core::ArenaSpan<bool> unread = arena.NewSpan<bool>(widths[i]);
      for (uint32_t c = 0; c < widths[i]; ++c) {
        unread[c] = !read[c];
      }
      if (auto* op = std::get_if<ex::Operator*>(&held)) {
        (*op)->SetUnreadColumns(unread);
      } else {
        std::get<ex::Breaker*>(held)->SetUnreadColumns(unread);
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

core::ArenaSpan<ex::SortKey> Lowering::QuerySortKeys(
    const std::vector<SortKey>& keys) const {
  auto lowered =
      arena().NewSpan<ex::SortKey>(static_cast<uint32_t>(keys.size()));
  for (uint32_t i = 0; i < keys.size(); ++i) {
    lowered[i] = {QueryPosition(keys[i].column), keys[i].descending};
  }
  return lowered;
}

void Lowering::Apply(const input_step::Sort& step) {
  core::ArenaSpan<ex::SortKey> keys = QuerySortKeys(step.keys);
  if (step.combine_first) {
    AddOperator(arena().New<ex::CombineBatches>());
  }
  if (step.keep) {
    AddOperator(
        arena().New<ex::TopK>(ex::TopKSpec{keys, context().pool, *step.keep}));
    return;
  }
  AddOperator(arena().New<ex::Sort>(ex::SortSpec{keys, context().pool}));
}

void Lowering::Apply(const input_step::Group& step) {
  ex::GroupedSortSpec spec{QueryPositions(step.keys),
                           QuerySortKeys(step.sorted_by)};
  if (step.combine_first) {
    AddOperator(arena().New<ex::CombineBatches>());
  }
  AddOperator(arena().New<ex::GroupedSort>(spec));
}

void Lowering::Apply(const input_step::NumberTree& step) {
  AddOperator(arena().New<ex::TreeNumberNodes>(QueryPosition(step.node),
                                               QueryPosition(step.parent)));
}

void Lowering::Apply(const input_step::WalkTree& step) {
  if (step.combine_first) {
    AddOperator(arena().New<ex::CombineBatches>());
  }
  uint32_t node = QueryPosition(step.node_number);
  uint32_t parent = QueryPosition(step.parent_number);
  if (step.direction == TreeDirection::kUp) {
    AddOperator(arena().New<ex::TreeChildFirst>(node, parent));
  } else {
    AddOperator(arena().New<ex::TreeParentFirst>(node, parent));
  }
}

void Lowering::Apply(const input_step::CombineBatches&) {
  AddOperator(arena().New<ex::CombineBatches>());
}

void Lowering::Apply(const input_step::Convert& step) {
  uint32_t position = QueryPosition(step.column);
  if (step.all_null) {
    AddOperator(arena().New<ex::NullColumn>(position, step.type));
    return;
  }
  ex::AssertTypeTarget target = core::String{};
  if (step.type.Is<core::Int64>()) {
    target = core::Int64{};
  } else if (step.type.Is<core::Double>()) {
    target = core::Double{};
  }
  PERFETTO_DCHECK(target.Is<core::String>() == step.type.Is<core::String>());
  AddOperator(arena().New<ex::AssertType>(
      position, target,
      arena().CopyString(
          plan().names().text(plan().columns()[step.column].name))));
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
  spec.key_columns = QueryPositions(keys);
  return spec;
}

void Lowering::SetSource(const core::exec::Source* source) {
  PERFETTO_DCHECK(!input_);
  input_ = source;
}

void Lowering::SetSparseColumns(std::vector<uint32_t> columns) {
  sparse_columns_ = std::move(columns);
}

void Lowering::AddOperator(LoweredStep operation) {
  operators_.push_back(operation);
}

Lowering& Lowering::AddSidePipeline() {
  plan_->lowerings_.push_back(std::make_unique<Lowering>(plan_));
  return *plan_->lowerings_.back();
}

uint32_t Lowering::FinishSide(const ex::Sink* sink) {
  return plan_->out_->pipelines_.Add(Build(nullptr), sink);
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

core::ArenaSpan<uint32_t> Lowering::QueryPositions(
    const std::vector<ColumnId>& ids) const {
  auto positions = arena().NewSpan<uint32_t>(static_cast<uint32_t>(ids.size()));
  for (uint32_t i = 0; i < ids.size(); ++i) {
    positions[i] = QueryPosition(ids[i]);
  }
  return positions;
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

const ex::Pipeline* Lowering::Build(const std::vector<uint32_t>* outputs) {
  // Chosen first: a sort serving in place keeps sparse columns unexpanded.
  if (outputs && !operators_.empty() &&
      context().sorted_outputs == LoweringContext::SortedOutputs::kInPlace) {
    auto* last = std::get_if<ex::Breaker*>(&operators_.back());
    if (last && (*last)->Is<ex::Sort>()) {
      static_cast<ex::Sort*>(*last)->ServeInPlace();
    }
  }
  PlaceExpands(std::move(sparse_columns_), outputs, context().sparse_outputs,
               arena(), &operators_);
  SetUnreadColumns(*source_columns_, outputs, arena(), &operators_);
  // Cut at each breaker, which the pipeline so far ends in.
  ex::Pipelines& pipelines = plan_->out_->pipelines_;
  const ex::Source* input = input_;
  std::vector<ex::Pipeline::Step> steps;
  for (LoweredStep step : operators_) {
    if (auto* t = std::get_if<ex::Transform*>(&step)) {
      steps.emplace_back(*t);
      continue;
    }
    if (auto* op = std::get_if<ex::Operator*>(&step)) {
      steps.emplace_back(*op);
      continue;
    }
    const ex::Pipeline* pipeline =
        arena().New<ex::Pipeline>(arena(), *input, steps);
    steps.clear();
    ex::Breaker* breaker = std::get<ex::Breaker*>(step);
    uint32_t id = pipelines.Add(pipeline, breaker);
    input = arena().New<ex::Served>(id, *breaker);
  }
  operators_.clear();
  return arena().New<ex::Pipeline>(arena(), *input, steps);
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
  out_->pipeline_ = lowering.Build(&outputs);
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
