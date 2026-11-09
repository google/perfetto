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
#include "src/trace_processor/core/exec/combine_batches.h"
#include "src/trace_processor/core/exec/expand.h"
#include "src/trace_processor/core/exec/grouped_sort.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/sort.h"
#include "src/trace_processor/core/exec/top_k.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/exec/tree_order.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;
namespace {

void Describe(const ex::Pipeline::Step& step, ex::StepColumns* out) {
  if (const auto* transform =
          std::get_if<std::unique_ptr<ex::Transform>>(&step)) {
    (*transform)->DescribeColumns(out);
  } else {
    std::get<std::unique_ptr<ex::Operator>>(step)->DescribeColumns(out);
  }
}

// Puts an Expand in front of each step which needs a `sparse` column of the
// source laid out, as late as it can be: a step reading the column but not
// sparse, or any operator which does not keep sparse columns, as one holds or
// rebuilds its batches. The columns still sparse after the steps are expanded
// for `outputs`, or all of them if it is null.
void PlaceExpands(std::vector<uint32_t> sparse,
                  const std::vector<uint32_t>* outputs,
                  std::vector<ex::Pipeline::Step>* steps) {
  if (sparse.empty()) {
    return;
  }
  std::vector<ex::Pipeline::Step> placed;
  for (ex::Pipeline::Step& step : *steps) {
    std::vector<uint32_t> expand;
    ex::StepColumns columns;
    Describe(step, &columns);
    if (std::holds_alternative<std::unique_ptr<ex::Operator>>(step) &&
        !columns.keeps_sparse) {
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
      placed.emplace_back(std::make_unique<ex::Expand>(std::move(expand)));
    }
    placed.push_back(std::move(step));
  }
  std::vector<uint32_t> expand;
  for (uint32_t column : sparse) {
    if (!outputs ||
        std::find(outputs->begin(), outputs->end(), column) != outputs->end()) {
      expand.push_back(column);
    }
  }
  if (!expand.empty()) {
    placed.emplace_back(std::make_unique<ex::Expand>(std::move(expand)));
  }
  *steps = std::move(placed);
}

// Tells each operator which columns of its input no later step reads, so an
// operator holding rows holds only the others. `steps` follow a source of
// `source_columns` columns. `outputs` are the columns read after them, or
// null if any may be.
void SetUnreadColumns(uint32_t source_columns,
                      const std::vector<uint32_t>* outputs,
                      std::vector<ex::Pipeline::Step>* steps) {
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
    auto* op = std::get_if<std::unique_ptr<ex::Operator>>(&(*steps)[i]);
    // Now by column of the step's input.
    if (keeps_all) {
      read.resize(widths[i]);
    } else {
      read.assign(widths[i], false);
    }
    for (uint32_t column : step.held_reads) {
      read[column] = true;
    }
    if (op && keeps_all) {
      std::vector<bool> unread(widths[i]);
      for (uint32_t c = 0; c < widths[i]; ++c) {
        unread[c] = !read[c];
      }
      (*op)->SetUnreadColumns(std::move(unread));
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

void Lowering::AddOperator(core::exec::Pipeline::Step operation) {
  operators_.push_back(std::move(operation));
}

Lowering& Lowering::AddSidePipeline() {
  plan_->pipelines_.push_back(std::make_unique<Lowering>(plan_));
  return *plan_->pipelines_.back();
}

const core::exec::Source* Lowering::FinishSide() {
  PhysicalPlan& out = *plan_->out_;
  std::unique_ptr<ex::Pipeline> pipeline = Build(nullptr);
  out.side_inputs_.push_back(std::move(input_));
  out.side_pipelines_.push_back(std::move(pipeline));
  return out.side_pipelines_.back().get();
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

std::unique_ptr<ex::Pipeline> Lowering::Build(
    const std::vector<uint32_t>* outputs) {
  // A caller reading sparse columns as lent needs none expanded for it.
  std::vector<uint32_t> none;
  bool lend = outputs &&
              context().sparse_outputs == LoweringContext::SparseOutputs::kLend;
  // Chosen first: a sort serving in place keeps sparse columns unexpanded.
  if (outputs && !operators_.empty() &&
      context().sorted_outputs == LoweringContext::SortedOutputs::kInPlace) {
    auto* last = std::get_if<std::unique_ptr<ex::Operator>>(&operators_.back());
    if (last && (*last)->Is<ex::Sort>()) {
      static_cast<ex::Sort&>(**last).ServeInPlace();
    }
  }
  PlaceExpands(std::move(sparse_columns_), lend ? &none : outputs, &operators_);
  SetUnreadColumns(*source_columns_, outputs, &operators_);
  return std::make_unique<ex::Pipeline>(*input_, std::move(operators_));
}

PlanLowering::PlanLowering(BoundPlan* bound, const LoweringContext& context)
    : bound_(*bound),
      context_(context),
      out_(std::make_unique<PhysicalPlan>()) {
  out_->pool_ = context.pool;
  pipelines_.push_back(std::make_unique<Lowering>(this));
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
  out_->input_ = std::move(lowering.input_);
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
