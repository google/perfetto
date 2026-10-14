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
#include "src/trace_processor/core/exec/grouped_sort.h"
#include "src/trace_processor/core/exec/sort.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/exec/tree_order.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;
namespace {

// Returns the type that both `a` and `b` convert to without loss. Returns
// nullopt if one is a string and the other a number.
//
// A column of unknown type is checked at runtime to hold the other side's
// type. For integers the check uses Int64, so it accepts any integer.
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

}  // namespace

Lowering::Lowering(const LogicalPlan& plan)
    : plan_(plan),
      out_(std::make_unique<PhysicalPlan>()),
      positions_(plan.columns().size(), std::numeric_limits<uint32_t>::max()),
      converted_(plan.columns().size()) {}

uint32_t Lowering::Position(ColumnId id) const {
  PERFETTO_DCHECK(positions_[id] != std::numeric_limits<uint32_t>::max());
  return positions_[id];
}

void Lowering::AddAscendingColumn(ColumnId column) {
  order_.ascending.push_back(column);
}

void Lowering::SetOrder(Order order) {
  order_ = std::move(order);
}

uint32_t Lowering::AllocateTemporaryColumn() {
  return column_count_++;
}

void Lowering::ResetLayout() {
  column_count_ = 0;
  order_ = Order();
  tree_columns_.reset();
  tree_direction_.reset();
}

Lowering::TreeColumns Lowering::PrepareTree(ColumnId node,
                                            ColumnId parent,
                                            TreeDirection direction) {
  if (!tree_columns_) {
    AddOperator(std::make_unique<ex::TreeNumberNodes>(Position(node),
                                                      Position(parent)));
    tree_columns_ = TreeColumns{column_count_++, column_count_++};
  }
  if (tree_direction_ == direction) {
    return *tree_columns_;
  }
  order_ = Order();
  if (direction == TreeDirection::kUp) {
    AddOperator(std::make_unique<ex::TreeChildFirst>(tree_columns_->node,
                                                     tree_columns_->parent));
  } else {
    AddOperator(std::make_unique<ex::TreeParentFirst>(tree_columns_->node,
                                                      tree_columns_->parent));
  }
  tree_direction_ = direction;
  return *tree_columns_;
}

void Lowering::Define(ColumnId id) {
  positions_[id] = column_count_++;
}

bool Lowering::has_source() const {
  return out_->input_ != nullptr;
}

void Lowering::SetSource(std::unique_ptr<core::exec::Source> source) {
  PERFETTO_DCHECK(!out_->input_);
  out_->input_ = std::move(source);
}

const core::exec::Source* Lowering::AddOperand(
    std::unique_ptr<core::exec::Source> source,
    std::vector<ex::Pipeline::Step> steps) {
  out_->operand_inputs_.push_back(std::move(source));
  out_->operand_pipelines_.push_back(std::make_unique<core::exec::Pipeline>(
      *out_->operand_inputs_.back(), std::move(steps),
      core::exec::ExecutionOptions()));
  return out_->operand_pipelines_.back().get();
}

void Lowering::LowerNode(PlanNodeId id) {
  const PlanNode& node = plan_.nodes()[id];
  node.operation().Lower(this, node);
}

void Lowering::PrepareGroups(const std::vector<ColumnId>& keys,
                             ColumnId ascending) {
  auto has = [](const std::vector<ColumnId>& ids, ColumnId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
  };
  bool grouped =
      std::is_permutation(order_.grouped_by.begin(), order_.grouped_by.end(),
                          keys.begin(), keys.end());
  if (grouped && has(order_.ascending, ascending)) {
    return;
  }
  // Use a single breaker so rows are copied once. It groups by `keys`, if
  // any, and sorts by `ascending` unless the rows are already sorted.
  std::vector<ex::SortKey> sort_keys;
  if (!order_.grouped_by.empty() || !has(order_.ascending, ascending)) {
    sort_keys.push_back({Position(ascending), false});
  }
  if (keys.empty()) {
    if (sort_keys.empty()) {
      return;
    }
    // The rows are reordered, so they no longer follow the tree.
    tree_direction_.reset();
    operators_.push_back(
        std::make_unique<ex::Sort>(ex::SortSpec{std::move(sort_keys)}));
    order_ = Order();
    order_.ascending.push_back(ascending);
    return;
  }
  ex::GroupedSortSpec spec;
  for (ColumnId key : keys) {
    spec.groups.push_back(Position(key));
  }
  spec.keys = std::move(sort_keys);
  bool sorts = !spec.keys.empty();
  tree_direction_.reset();
  operators_.push_back(std::make_unique<ex::GroupedSort>(std::move(spec)));
  // Grouping is stable, so without a sort, any known order is kept.
  if (sorts) {
    order_.ascending = {ascending};
  }
  order_.grouped_by = keys;
  order_.group_column = column_count_++;
}

void Lowering::SortRows(const std::vector<SortKey>& keys) {
  PERFETTO_DCHECK(!keys.empty());
  const auto& sorted = order_.ascending;
  if (keys.size() == 1 && !keys[0].descending && order_.grouped_by.empty() &&
      std::find(sorted.begin(), sorted.end(), keys[0].column) != sorted.end()) {
    return;
  }
  ex::SortSpec sort;
  for (const SortKey& key : keys) {
    if (!plan_.columns()[key.column].type.storage()) {
      Require(key.column, core::Int64{});
    }
    sort.keys.push_back({Position(key.column), key.descending});
  }
  operators_.push_back(std::make_unique<ex::Sort>(std::move(sort)));
  order_ = Order();
  if (!keys[0].descending) {
    order_.ascending.push_back(keys[0].column);
  }
  // The rows no longer follow the tree, though their numbering still holds.
  tree_direction_.reset();
}

void Lowering::AddOperator(core::exec::Pipeline::Step operation) {
  operators_.push_back(std::move(operation));
}

void Lowering::Require(ColumnId column, core::StorageType type) {
  Require({column, Position(column), &operators_}, type);
}

void Lowering::Require(const Side& side, core::StorageType type) {
  std::optional<PlanType> common = CommonType(TypeOf(side.column), type);
  if (!common || !common->storage() || !(*common->storage() == type)) {
    const char* what = type.Is<core::String>()   ? "strings"
                       : type.Is<core::Double>() ? "numbers"
                                                 : "integers";
    Fail(base::ErrStatus("column '%s' must hold %s",
                         plan_.columns()[side.column].name.c_str(), what));
    return;
  }
  Convert(side, type);
}

PlanType Lowering::Unify(const std::vector<Side>& sides) {
  PlanType common = PlanType::Null();
  for (const Side& side : sides) {
    std::optional<PlanType> next = CommonType(common, TypeOf(side.column));
    if (!next) {
      Fail(base::ErrStatus(
          "column '%s' holds strings in one input and numbers in another",
          plan_.columns()[side.column].name.c_str()));
      return common;
    }
    common = *next;
  }
  for (const Side& side : sides) {
    Convert(side, common);
  }
  return common;
}

PlanType Lowering::TypeOf(ColumnId column) const {
  if (converted_[column]) {
    return *converted_[column];
  }
  return plan_.columns()[column].type;
}

void Lowering::Convert(const Side& side, const PlanType& type) {
  PlanType from = TypeOf(side.column);
  if (!type.storage() ||
      (from.storage() && *from.storage() == *type.storage())) {
    return;
  }
  converted_[side.column] = *type.storage();
  // TypeOf forgets a Null column once it has been converted.
  if (plan_.columns()[side.column].type.is_null()) {
    side.steps->push_back(
        std::make_unique<ex::NullColumn>(side.position, *type.storage()));
    return;
  }
  ex::AssertTypeTarget target = core::String{};
  if (type.Is<core::Int64>()) {
    target = core::Int64{};
  } else if (type.Is<core::Double>()) {
    target = core::Double{};
  }
  PERFETTO_DCHECK(target.Is<core::String>() == type.Is<core::String>());
  side.steps->push_back(std::make_unique<ex::AssertType>(
      side.position, target, plan_.columns()[side.column].name));
}

void Lowering::Fail(base::Status status) {
  if (status_.ok()) {
    status_ = std::move(status);
  }
}

base::StatusOr<std::unique_ptr<PhysicalPlan>> Lowering::Finish() {
  RETURN_IF_ERROR(status_);
  out_->pipeline_ = std::make_unique<ex::Pipeline>(
      *out_->input_, std::move(operators_), ex::ExecutionOptions());
  for (const NamedColumn& column : plan_.output()) {
    out_->columns_.push_back({column.name, Position(column.id)});
  }
  return std::move(out_);
}

PhysicalPlan::PhysicalPlan() = default;
PhysicalPlan::~PhysicalPlan() = default;

base::StatusOr<std::unique_ptr<PhysicalPlan>> Lower(const LogicalPlan& plan) {
  Lowering lowering(plan);
  lowering.LowerNode(plan.root());
  return lowering.Finish();
}

}  // namespace perfetto::trace_processor::pipeline
