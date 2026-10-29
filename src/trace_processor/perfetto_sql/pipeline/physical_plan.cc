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

// Rewrites the subtree at `id`, inputs first.
void RewriteSubtree(BoundRewriter* rewriter, PlanNodeId id) {
  std::vector<PlanNodeId> children = rewriter->plan().nodes()[id].children();
  for (PlanNodeId child : children) {
    RewriteSubtree(rewriter, child);
  }
  rewriter->plan().nodes()[id].operation().RewriteBound(rewriter, id);
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

Lowering::Lowering(PlanLowering* plan)
    : plan_(plan),
      positions_(plan->plan().columns().size(),
                 std::numeric_limits<uint32_t>::max()) {}

Lowering::~Lowering() = default;

void Lowering::LowerNode(PlanNodeId id) {
  const PlanNode& node = plan().nodes()[id];
  node.operation().Lower(this, node);
}

void Lowering::Fail(base::Status status) {
  plan_->Fail(std::move(status));
}

void Lowering::SetSource(std::unique_ptr<core::exec::Source> source) {
  PERFETTO_DCHECK(!input_);
  input_ = std::move(source);
  // Every operation declares its source's columns before setting it.
  source_columns_ = column_count_;
}

void Lowering::AddOperator(core::exec::Pipeline::Step operation) {
  order_.DeclareOrderRead();
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
  order_.RequireSorted(keys, this);
}

void Lowering::RequireGrouped(const std::vector<ColumnId>& keys) {
  order_.RequireGrouped(keys, this);
}

void Lowering::RequireSortedWithinGroups(
    const std::vector<ColumnId>& keys,
    const std::vector<SortKey>& sorted_by) {
  order_.RequireSortedWithinGroups(keys, sorted_by, this);
}

uint32_t Lowering::RequireGroupNumbers(const std::vector<ColumnId>& keys) {
  return group_number_positions_[order_.RequireGroupNumbers(keys, this)];
}

void Lowering::RequireFullBatches() {
  order_.RequireFullBatches(this);
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
  order_.RequireTreeWalk(node, parent, direction, this);
  return *tree_columns_;
}

void Lowering::DeclareColumn(ColumnId id) {
  positions_[id] = column_count_++;
}

uint32_t Lowering::DeclareTemporaryColumn() {
  return column_count_++;
}

void Lowering::DeclareType(ColumnId id, core::StorageType type) {
  plan_->converted_[id] = type;
}

void Lowering::DeclareSorted(const std::vector<SortKey>& keys) {
  order_.DeclareSorted(keys);
}

void Lowering::DeclareGrouped(const std::vector<ColumnId>& keys) {
  order_.DeclareGrouped(keys);
}

void Lowering::DeclareSortedWithinGroups(
    const std::vector<ColumnId>& keys,
    const std::vector<SortKey>& sorted_by) {
  order_.DeclareSortedWithinGroups(keys, sorted_by);
}

void Lowering::DeclareSmallBatches() {
  order_.DeclareSmallBatches();
}

void Lowering::DeclareTreeWalk(ColumnId node,
                               ColumnId parent,
                               TreeColumns columns,
                               TreeDirection direction) {
  tree_columns_ = columns;
  order_.DeclareTreeWalk(node, parent, direction);
}

void Lowering::DeclareRowsDropped() {
  order_.DeclareRowsDropped();
  tree_columns_.reset();
}

void Lowering::DeclareGroupNumbers(const std::vector<ColumnId>& keys,
                                   uint32_t position) {
  order_.DeclareGroupNumbers(keys);
  group_number_positions_.push_back(position);
}

void Lowering::ResetLayout() {
  column_count_ = 0;
  order_.Reset();
  tree_columns_.reset();
  group_number_positions_.clear();
}

uint32_t Lowering::QueryPosition(ColumnId id) const {
  PERFETTO_DCHECK(positions_[id] != std::numeric_limits<uint32_t>::max());
  return positions_[id];
}

PlanType Lowering::QueryType(ColumnId id) const {
  return plan_->TypeOf(id);
}

std::vector<SortKey> Lowering::QuerySortedBy(
    const std::vector<ColumnId>& columns) const {
  return order_.SortedBy(columns);
}

const LogicalPlan& Lowering::plan() const {
  return plan_->plan();
}

void Lowering::AddOrderIndependent(core::exec::Pipeline::Step step) {
  operators_.push_back(std::move(step));
}

void Lowering::MakeComparable(const std::vector<SortKey>& keys) {
  for (const SortKey& key : keys) {
    if (!plan_->TypeOf(key.column).storage()) {
      RequireType(key.column, core::Int64{});
    }
  }
}

void Lowering::Sort(const std::vector<SortKey>& keys) {
  ex::SortSpec sort;
  sort.pool = context().pool;
  for (const SortKey& key : keys) {
    sort.keys.push_back({QueryPosition(key.column), key.descending});
  }
  open_sort_index_ = operators_.size();
  operators_.push_back(std::make_unique<ex::Sort>(std::move(sort)));
}

void Lowering::RemoveSort() {
  operators_.erase(operators_.begin() +
                   static_cast<ptrdiff_t>(open_sort_index_));
}

void Lowering::GroupedSort(const std::vector<ColumnId>& keys,
                           const std::vector<SortKey>& sorted_by) {
  ex::GroupedSortSpec spec;
  for (ColumnId key : keys) {
    spec.groups.push_back(QueryPosition(key));
  }
  for (const SortKey& key : sorted_by) {
    spec.keys.push_back({QueryPosition(key.column), key.descending});
  }
  operators_.push_back(std::make_unique<ex::GroupedSort>(std::move(spec)));
  group_number_positions_.push_back(column_count_++);
}

void Lowering::NumberTree(ColumnId node, ColumnId parent) {
  AddOrderIndependent(std::make_unique<ex::TreeNumberNodes>(
      QueryPosition(node), QueryPosition(parent)));
  tree_columns_ = TreeColumns{column_count_++, column_count_++};
}

void Lowering::WalkTree(TreeDirection direction) {
  if (direction == TreeDirection::kUp) {
    operators_.push_back(std::make_unique<ex::TreeChildFirst>(
        tree_columns_->node, tree_columns_->parent));
  } else {
    operators_.push_back(std::make_unique<ex::TreeParentFirst>(
        tree_columns_->node, tree_columns_->parent));
  }
}

void Lowering::CombineBatches() {
  AddOrderIndependent(std::make_unique<ex::CombineBatches>());
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
  if (plan.plan().columns()[side.column].type.is_null()) {
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
  SetUnreadColumns(source_columns_, outputs, &operators_);
  return std::make_unique<ex::Pipeline>(*input_, std::move(operators_));
}

PlanLowering::PlanLowering(const LogicalPlan& plan,
                           const LoweringContext& context)
    : plan_(plan),
      context_(context),
      converted_(plan.columns().size()),
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
  for (const NamedColumn& column : plan_.output()) {
    outputs.push_back(lowering.QueryPosition(column.id));
    out_->columns_.push_back({plan_.names().text(column.name), outputs.back()});
  }
  out_->pipeline_ = lowering.Build(&outputs);
  out_->input_ = std::move(lowering.input_);
  return std::move(out_);
}

PlanType PlanLowering::TypeOf(ColumnId column) const {
  if (converted_[column]) {
    return *converted_[column];
  }
  return plan_.columns()[column].type;
}

PhysicalPlan::PhysicalPlan() = default;
PhysicalPlan::~PhysicalPlan() = default;

std::optional<RowOrder> BoundRewriter::QueryRowOrder(PlanNodeId node) const {
  PlanLowering lowering(*plan_, context_);
  lowering.main().LowerNode(node);
  if (!lowering.status().ok()) {
    return std::nullopt;
  }
  return lowering.main().QueryOrder();
}

base::StatusOr<std::unique_ptr<PhysicalPlan>> Lower(
    LogicalPlan plan,
    const LoweringContext& context) {
  BoundRewriter rewriter(&plan, context);
  RewriteSubtree(&rewriter, plan.root());
  PlanLowering lowering(plan, context);
  lowering.main().LowerNode(plan.root());
  return lowering.Finish();
}

}  // namespace perfetto::trace_processor::pipeline
