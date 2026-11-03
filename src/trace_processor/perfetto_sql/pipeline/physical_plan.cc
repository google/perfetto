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
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/assert_type.h"
#include "src/trace_processor/core/exec/combine_batches.h"
#include "src/trace_processor/core/exec/grouped_sort.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/sort.h"
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
    if (op && keeps_all) {
      std::vector<bool> unread(widths[i]);
      for (uint32_t c = 0; c < widths[i]; ++c) {
        unread[c] = !read[c];
      }
      (*op)->SetUnreadColumns(std::move(unread));
    }
    if (keeps_all) {
      read.resize(widths[i]);
    } else {
      read.assign(widths[i], false);
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

Lowering::Lowering(PlanLowering* plan) : plan_(plan) {
  output_.positions.assign(plan->plan().columns().size(),
                           OutputProperties::kNoPosition);
}

Lowering::~Lowering() = default;

void Lowering::LowerNode(PlanNodeId id) {
  const PlanNode& node = plan().nodes()[id];
  node.operation().Lower(this, node);
  output_ = plan_->bound_.outputs[id];
  if (input_ && !source_columns_) {
    source_columns_ = output_.column_count;
  }
  if (!output_.before_open_sort) {
    open_sort_.reset();
  }
}

void Lowering::Fail(base::Status status) {
  plan_->Fail(std::move(status));
}

void Lowering::SetSource(std::unique_ptr<core::exec::Source> source) {
  PERFETTO_DCHECK(!input_);
  input_ = std::move(source);
}

void Lowering::AddOperator(core::exec::Pipeline::Step operation) {
  CloseOpenSort();
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

uint32_t Lowering::AddFilterValue(core::exec::FilterValue value) {
  std::vector<core::exec::FilterValue>& values = plan_->out_->filter_values_;
  values.push_back(std::move(value));
  return static_cast<uint32_t>(values.size() - 1);
}

const LoweringContext& Lowering::context() const {
  return plan_->context_;
}

void Lowering::RequireType(ColumnId column, core::StorageType type) {
  std::optional<PlanType> common =
      CommonType(plan_->TypeOf(column), PlanType(type));
  if (!common || !common->storage() || !(*common->storage() == type)) {
    const char* what = type.Is<core::String>()   ? "strings"
                       : type.Is<core::Double>() ? "numbers"
                                                 : "integers";
    Fail(base::ErrStatus(
        "column '%s' must hold %s",
        plan().names().text(plan().columns()[column].name).c_str(), what));
    return;
  }
  Convert({this, column}, type);
}

PlanType Lowering::RequireCommonType(const std::vector<Side>& sides) {
  PlanType common = PlanType::Null();
  for (const Side& side : sides) {
    std::optional<PlanType> next =
        CommonType(common, plan_->TypeOf(side.column));
    if (!next) {
      Fail(base::ErrStatus(
          "column '%s' holds strings in one input and numbers in another",
          plan().names().text(plan().columns()[side.column].name).c_str()));
      return common;
    }
    common = *next;
  }
  for (const Side& side : sides) {
    Convert(side, common);
  }
  return common;
}

void Lowering::RequireSorted(const std::vector<SortKey>& keys) {
  OutputProperties::RequiredSteps steps = output_.RequireSorted(keys);
  if (steps.drop_open_sort) {
    EraseOpenSort();
  }
  if (!steps.reorder) {
    return;
  }
  ex::SortSpec sort;
  sort.pool = context().pool;
  for (const SortKey& key : keys) {
    if (!plan_->TypeOf(key.column).storage()) {
      RequireType(key.column, core::Int64{});
    }
    sort.keys.push_back({QueryPosition(key.column), key.descending});
  }
  if (steps.combine_batches) {
    AddOrderIndependent(std::make_unique<ex::CombineBatches>());
  }
  open_sort_ = operators_.size();
  operators_.push_back(std::make_unique<ex::Sort>(std::move(sort)));
}

void Lowering::RequireGrouped(const std::vector<ColumnId>& keys) {
  if (output_.IsArranged(keys, {})) {
    return;
  }
  if (DropOpenSort() && output_.IsArranged(keys, {})) {
    return;
  }
  GroupRows(keys, {});
}

void Lowering::RequireSortedWithinGroups(
    const std::vector<ColumnId>& keys,
    const std::vector<SortKey>& sorted_by) {
  // Without keys, all rows are one group, so this is a plain sort.
  if (keys.empty()) {
    RequireSorted(sorted_by);
    return;
  }
  if (output_.IsArranged(keys, sorted_by)) {
    return;
  }
  if (DropOpenSort() && output_.IsArranged(keys, sorted_by)) {
    return;
  }
  // Grouping is stable, so if the rows are already sorted by `sorted_by`,
  // grouping them is enough.
  GroupRows(keys, output_.IsArranged({}, sorted_by) ? std::vector<SortKey>()
                                                    : sorted_by);
}

uint32_t Lowering::RequireGroupNumbers(const std::vector<ColumnId>& keys) {
  RequireGrouped(keys);
  for (const OutputProperties::GroupNumbers& numbers : output_.group_numbers) {
    if (std::is_permutation(numbers.keys.begin(), numbers.keys.end(),
                            keys.begin(), keys.end())) {
      return numbers.position;
    }
  }
  // The rows are grouped but have no group numbers. Grouping them again
  // adds the numbers.
  GroupRows(keys, {});
  return output_.group_numbers.back().position;
}

void Lowering::RequireFullBatches() {
  if (!output_.full_batches) {
    AddOrderIndependent(std::make_unique<ex::CombineBatches>());
    output_.full_batches = true;
  }
}

core::exec::IntervalLaneSpec Lowering::RequireIntervalLanes(
    ColumnId ts,
    ColumnId dur,
    const std::vector<ColumnId>& keys) {
  RequireType(ts, core::Int64{});
  RequireType(dur, core::Int64{});
  RequireSortedWithinGroups(keys, {{ts, false}});
  core::exec::IntervalLaneSpec spec;
  if (!keys.empty()) {
    spec.group_column = RequireGroupNumbers(keys);
  }
  spec.ts_column = QueryPosition(ts);
  spec.dur_column = QueryPosition(dur);
  for (ColumnId key : keys) {
    spec.key_columns.push_back(QueryPosition(key));
  }
  return spec;
}

Lowering::TreeColumns Lowering::RequireTreeWalk(ColumnId node,
                                                ColumnId parent,
                                                TreeDirection direction) {
  uint32_t node_position = QueryPosition(node);
  uint32_t parent_position = QueryPosition(parent);
  OutputProperties::RequiredSteps steps =
      output_.RequireTreeWalk(node, parent, direction);
  if (steps.drop_open_sort) {
    EraseOpenSort();
  }
  TreeColumns columns = QueryTreeColumns();
  if (steps.number_tree) {
    AddOrderIndependent(
        std::make_unique<ex::TreeNumberNodes>(node_position, parent_position));
  }
  if (steps.combine_batches) {
    AddOrderIndependent(std::make_unique<ex::CombineBatches>());
  }
  if (steps.reorder) {
    open_sort_.reset();
    if (direction == TreeDirection::kUp) {
      operators_.push_back(
          std::make_unique<ex::TreeChildFirst>(columns.node, columns.parent));
    } else {
      operators_.push_back(
          std::make_unique<ex::TreeParentFirst>(columns.node, columns.parent));
    }
  }
  return columns;
}

void Lowering::DeclareType(ColumnId id, core::StorageType type) {
  plan_->converted_[id] = type;
}

Lowering::TreeColumns Lowering::QueryTreeColumns() const {
  return {output_.tree->node_position, output_.tree->parent_position};
}

uint32_t Lowering::QueryPosition(ColumnId id) const {
  PERFETTO_DCHECK(output_.positions[id] != OutputProperties::kNoPosition);
  return output_.positions[id];
}

PlanType Lowering::QueryType(ColumnId id) const {
  return plan_->TypeOf(id);
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

void Lowering::AddOrderIndependent(core::exec::Pipeline::Step step) {
  operators_.push_back(std::move(step));
}

void Lowering::AddOrderDependent(core::exec::Pipeline::Step step) {
  CloseOpenSort();
  operators_.push_back(std::move(step));
}

void Lowering::AddBreaker(core::exec::Pipeline::Step breaker) {
  RequireFullBatches();
  CloseOpenSort();
  operators_.push_back(std::move(breaker));
}

bool Lowering::DropOpenSort() {
  if (!output_.DropOpenSort()) {
    return false;
  }
  EraseOpenSort();
  return true;
}

void Lowering::EraseOpenSort() {
  operators_.erase(operators_.begin() + static_cast<ptrdiff_t>(*open_sort_));
  open_sort_.reset();
}

void Lowering::CloseOpenSort() {
  open_sort_.reset();
  output_.CloseOpenSort();
}

void Lowering::GroupRows(const std::vector<ColumnId>& keys,
                         const std::vector<SortKey>& sorted_by) {
  ex::GroupedSortSpec spec;
  for (ColumnId key : keys) {
    spec.groups.push_back(QueryPosition(key));
  }
  for (const SortKey& key : sorted_by) {
    if (!plan_->TypeOf(key.column).storage()) {
      RequireType(key.column, core::Int64{});
    }
    spec.keys.push_back({QueryPosition(key.column), key.descending});
  }
  AddBreaker(std::make_unique<ex::GroupedSort>(std::move(spec)));
  output_.Group(keys, sorted_by);
}

void Lowering::Convert(const Side& side, const PlanType& type) {
  PlanLowering& plan = *side.pipeline->plan_;
  PlanType from = plan.TypeOf(side.column);
  if (!type.storage() ||
      (from.storage() && *from.storage() == *type.storage())) {
    return;
  }
  plan.converted_[side.column] = *type.storage();
  uint32_t position = side.pipeline->QueryPosition(side.column);
  // TypeOf forgets a Null column once it has been converted.
  if (plan.bound_.bindings.types[side.column].is_null()) {
    side.pipeline->AddOrderIndependent(
        std::make_unique<ex::NullColumn>(position, *type.storage()));
    return;
  }
  ex::AssertTypeTarget target = core::String{};
  if (type.Is<core::Int64>()) {
    target = core::Int64{};
  } else if (type.Is<core::Double>()) {
    target = core::Double{};
  }
  PERFETTO_DCHECK(target.Is<core::String>() == type.Is<core::String>());
  side.pipeline->AddOrderIndependent(std::make_unique<ex::AssertType>(
      position, target,
      plan.plan().names().text(plan.plan().columns()[side.column].name)));
}

std::unique_ptr<ex::Pipeline> Lowering::Build(
    const std::vector<uint32_t>* outputs) {
  SetUnreadColumns(*source_columns_, outputs, &operators_);
  return std::make_unique<ex::Pipeline>(*input_, std::move(operators_));
}

PlanLowering::PlanLowering(const BoundPlan& bound,
                           const LoweringContext& context)
    : bound_(bound),
      context_(context),
      converted_(bound.plan.columns().size()),
      out_(std::make_unique<PhysicalPlan>()) {
  pipelines_.push_back(std::make_unique<Lowering>(this));
}

PlanLowering::~PlanLowering() = default;

void PlanLowering::Fail(base::Status status) {
  if (status_.ok()) {
    status_ = std::move(status);
  }
}

base::StatusOr<std::unique_ptr<PhysicalPlan>> PlanLowering::Finish() {
  RETURN_IF_ERROR(status_);
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

PlanType PlanLowering::TypeOf(ColumnId column) const {
  if (converted_[column]) {
    return *converted_[column];
  }
  return bound_.bindings.types[column];
}

PhysicalPlan::PhysicalPlan() = default;
PhysicalPlan::~PhysicalPlan() = default;

base::StatusOr<std::unique_ptr<PhysicalPlan>> Lower(
    const BoundPlan& bound,
    const LoweringContext& context) {
  PlanLowering lowering(bound, context);
  lowering.main().LowerNode(bound.plan.root());
  return lowering.Finish();
}

}  // namespace perfetto::trace_processor::pipeline
