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
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/assert_type.h"
#include "src/trace_processor/core/exec/combine_batches.h"
#include "src/trace_processor/core/exec/grouped_sort.h"
#include "src/trace_processor/core/exec/sort.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/exec/tree_order.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;
namespace {

bool Contains(const std::vector<ColumnId>& columns, ColumnId column) {
  return std::find(columns.begin(), columns.end(), column) != columns.end();
}

// Returns `keys` without the keys on columns in `drop`.
std::vector<SortKey> Without(const std::vector<SortKey>& keys,
                             const std::vector<ColumnId>& drop) {
  std::vector<SortKey> kept;
  for (const SortKey& key : keys) {
    if (!Contains(drop, key.column)) {
      kept.push_back(key);
    }
  }
  return kept;
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
}

void Lowering::AddOperator(core::exec::Pipeline::Step operation) {
  open_sort_.reset();
  operators_.push_back(std::move(operation));
}

Lowering& Lowering::AddSidePipeline() {
  plan_->pipelines_.push_back(std::make_unique<Lowering>(plan_));
  return *plan_->pipelines_.back();
}

const core::exec::Source* Lowering::FinishSide() {
  PhysicalPlan& out = *plan_->out_;
  std::unique_ptr<ex::Pipeline> pipeline = Build();
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
  if (IsArranged({}, keys)) {
    return;
  }
  if (DropOpenSort() && IsArranged({}, keys)) {
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
  AddSort(std::make_unique<ex::Sort>(std::move(sort)), {{{}, keys}});
}

void Lowering::RequireGrouped(const std::vector<ColumnId>& keys) {
  if (IsArranged(keys, {})) {
    return;
  }
  if (DropOpenSort() && IsArranged(keys, {})) {
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
  if (IsArranged(keys, sorted_by)) {
    return;
  }
  if (DropOpenSort() && IsArranged(keys, sorted_by)) {
    return;
  }
  // Grouping is stable, so if the rows are already sorted by `sorted_by`,
  // grouping them is enough.
  GroupRows(keys,
            IsArranged({}, sorted_by) ? std::vector<SortKey>() : sorted_by);
}

uint32_t Lowering::RequireGroupNumbers(const std::vector<ColumnId>& keys) {
  RequireGrouped(keys);
  for (const GroupNumbers& numbers : group_numbers_) {
    if (std::is_permutation(numbers.keys.begin(), numbers.keys.end(),
                            keys.begin(), keys.end())) {
      return numbers.position;
    }
  }
  // The rows are grouped but have no group numbers. Grouping them again
  // adds the numbers.
  GroupRows(keys, {});
  return group_numbers_.back().position;
}

void Lowering::RequireFullBatches() {
  if (!full_batches_) {
    AddOrderIndependent(std::make_unique<ex::CombineBatches>());
    full_batches_ = true;
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
  bool same_tree =
      tree_columns_ && tree_node_ == node && tree_parent_ == parent;
  if (same_tree && tree_walk_ == direction) {
    return *tree_columns_;
  }
  if (same_tree && DropOpenSort() && tree_walk_ == direction) {
    return *tree_columns_;
  }
  if (!same_tree) {
    AddOrderIndependent(std::make_unique<ex::TreeNumberNodes>(
        QueryPosition(node), QueryPosition(parent)));
    tree_columns_ = TreeColumns{column_count_++, column_count_++};
    tree_node_ = node;
    tree_parent_ = parent;
  }
  if (direction == TreeDirection::kUp) {
    AddBreaker(std::make_unique<ex::TreeChildFirst>(tree_columns_->node,
                                                    tree_columns_->parent));
  } else {
    // Rows are output as their parents arrive, so batches can be small.
    AddOperator(std::make_unique<ex::TreeParentFirst>(tree_columns_->node,
                                                      tree_columns_->parent));
    DeclareSmallBatches();
  }
  arrangements_.clear();
  tree_walk_ = direction;
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
  arrangements_.push_back({{}, keys});
}

void Lowering::DeclareGrouped(const std::vector<ColumnId>& keys) {
  arrangements_.push_back({keys, {}});
}

void Lowering::DeclareSortedWithinGroups(
    const std::vector<ColumnId>& keys,
    const std::vector<SortKey>& sorted_by) {
  arrangements_.push_back({keys, sorted_by});
}

void Lowering::DeclareSmallBatches() {
  full_batches_ = false;
}

void Lowering::DeclareTreeWalk(ColumnId node,
                               ColumnId parent,
                               TreeColumns columns,
                               TreeDirection direction) {
  tree_columns_ = columns;
  tree_node_ = node;
  tree_parent_ = parent;
  tree_walk_ = direction;
}

void Lowering::DeclareRowsDropped() {
  tree_walk_.reset();
  tree_columns_.reset();
}

void Lowering::DeclareGroupNumbers(const std::vector<ColumnId>& keys,
                                   uint32_t position) {
  group_numbers_.push_back({keys, position});
}

void Lowering::ResetLayout() {
  column_count_ = 0;
  open_sort_.reset();
  full_batches_ = true;
  arrangements_.clear();
  tree_walk_.reset();
  tree_columns_.reset();
  group_numbers_.clear();
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
  std::vector<SortKey> longest;
  for (const Arrangement& arrangement : arrangements_) {
    if (!arrangement.grouped_by.empty()) {
      continue;
    }
    std::vector<SortKey> keys;
    for (const SortKey& key : arrangement.sorted_by) {
      if (!Contains(columns, key.column)) {
        break;
      }
      keys.push_back(key);
    }
    if (keys.size() > longest.size()) {
      longest = std::move(keys);
    }
  }
  return longest;
}

const LogicalPlan& Lowering::plan() const {
  return plan_->plan();
}

bool Lowering::IsArranged(const std::vector<ColumnId>& keys,
                          const std::vector<SortKey>& sorted_by) const {
  // Sort keys on group columns are constant within a group, so ignore them.
  std::vector<SortKey> wanted = Without(sorted_by, keys);
  if (keys.empty() && wanted.empty()) {
    return true;
  }
  for (const Arrangement& arrangement : arrangements_) {
    // An arrangement grouped by a subset of `keys` can still match if it is
    // sorted by the remaining keys first.
    if (!std::all_of(arrangement.grouped_by.begin(),
                     arrangement.grouped_by.end(),
                     [&](ColumnId column) { return Contains(keys, column); })) {
      continue;
    }
    std::vector<SortKey> sorted =
        Without(arrangement.sorted_by, arrangement.grouped_by);
    std::vector<ColumnId> missing = arrangement.grouped_by;
    size_t next = 0;
    while (next < sorted.size() && Contains(keys, sorted[next].column)) {
      missing.push_back(sorted[next++].column);
    }
    if (!std::is_permutation(missing.begin(), missing.end(), keys.begin(),
                             keys.end())) {
      continue;
    }
    std::vector<SortKey> rest(sorted.begin() + static_cast<ptrdiff_t>(next),
                              sorted.end());
    if (rest.size() >= wanted.size() &&
        std::equal(wanted.begin(), wanted.end(), rest.begin(),
                   [](const SortKey& a, const SortKey& b) {
                     return a.column == b.column &&
                            a.descending == b.descending;
                   })) {
      return true;
    }
  }
  return false;
}

void Lowering::AddOrderIndependent(core::exec::Pipeline::Step step) {
  operators_.push_back(std::move(step));
}

void Lowering::AddSort(core::exec::Pipeline::Step sort,
                       std::vector<Arrangement> arrangements) {
  RequireFullBatches();
  open_sort_ =
      OpenSort{operators_.size(), std::move(arrangements_), tree_walk_};
  arrangements_ = std::move(arrangements);
  tree_walk_.reset();
  operators_.push_back(std::move(sort));
}

void Lowering::AddOrderDependent(core::exec::Pipeline::Step step) {
  open_sort_.reset();
  operators_.push_back(std::move(step));
}

void Lowering::AddBreaker(core::exec::Pipeline::Step breaker) {
  RequireFullBatches();
  open_sort_.reset();
  operators_.push_back(std::move(breaker));
}

bool Lowering::DropOpenSort() {
  if (!open_sort_) {
    return false;
  }
  operators_.erase(operators_.begin() +
                   static_cast<ptrdiff_t>(open_sort_->index));
  arrangements_ = std::move(open_sort_->arrangements);
  tree_walk_ = open_sort_->tree_walk;
  open_sort_.reset();
  return true;
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
  std::vector<Arrangement> kept;
  if (sorted_by.empty()) {
    // Grouping is stable, so an order known within groups of a subset of
    // `keys` still holds within the new groups.
    for (const Arrangement& arrangement : arrangements_) {
      if (std::all_of(
              arrangement.grouped_by.begin(), arrangement.grouped_by.end(),
              [&](ColumnId column) { return Contains(keys, column); })) {
        kept.push_back({keys, arrangement.sorted_by});
      }
    }
  }
  kept.push_back({keys, sorted_by});
  arrangements_ = std::move(kept);
  tree_walk_.reset();
  group_numbers_.push_back({keys, column_count_++});
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

std::unique_ptr<ex::Pipeline> Lowering::Build() {
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
  out_->pipeline_ = lowering.Build();
  out_->input_ = std::move(lowering.input_);
  for (const NamedColumn& column : plan_.output()) {
    out_->columns_.push_back(
        {plan_.names().text(column.name), lowering.QueryPosition(column.id)});
  }
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

base::StatusOr<std::unique_ptr<PhysicalPlan>> Lower(
    const LogicalPlan& plan,
    const LoweringContext& context) {
  PlanLowering lowering(plan, context);
  lowering.main().LowerNode(plan.root());
  return lowering.Finish();
}

}  // namespace perfetto::trace_processor::pipeline
