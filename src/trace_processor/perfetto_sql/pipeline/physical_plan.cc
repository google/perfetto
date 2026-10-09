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
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/assert_type.h"
#include "src/trace_processor/core/exec/group_by.h"
#include "src/trace_processor/core/exec/sort.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/exec/tree_order.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

Lowering::Lowering(const LogicalPlan& plan)
    : plan_(plan),
      out_(std::make_unique<PhysicalPlan>()),
      positions_(plan.columns().size(), std::numeric_limits<uint32_t>::max()),
      int64_columns_(plan.columns().size(), false) {}

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
  // GroupBy is stable, so sorting first orders every group.
  if (!order_.grouped_by.empty() || !has(order_.ascending, ascending)) {
    ex::SortSpec sort;
    sort.keys.push_back({Position(ascending), false});
    operators_.push_back(std::make_unique<ex::Sort>(std::move(sort)));
    order_ = Order();
    order_.ascending.push_back(ascending);
  }
  if (!keys.empty()) {
    std::vector<uint32_t> positions;
    for (ColumnId key : keys) {
      positions.push_back(Position(key));
    }
    operators_.push_back(std::make_unique<ex::GroupBy>(std::move(positions)));
    order_.grouped_by = keys;
    order_.group_column = column_count_++;
  }
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
    if (!plan_.columns()[key.column].type) {
      RequireInt64(key.column);
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

void Lowering::RequireInt64(ColumnId column) {
  RequireInt64(column, Position(column), &operators_);
}

void Lowering::RequireInt64(ColumnId column,
                            uint32_t position,
                            std::vector<ex::Pipeline::Step>* into) {
  // TODO(lalitm): Replace this with numeric normalization and support Double
  // totals in tree accumulation. For dynamically typed inputs, a Double in a
  // later batch may require promoting earlier Int64 values too. Choosing one
  // numeric type for the whole column may therefore need a breaker that buffers
  // input before emitting any values. Update logical result typing alongside
  // the executor so SUM is no longer restricted to integers.
  const auto& type = plan_.columns()[column].type;
  if ((type && type->Is<core::Int64>()) || int64_columns_[column]) {
    return;
  }
  into->push_back(std::make_unique<ex::AssertType>(
      position, ex::AssertTypeTarget{core::Int64{}},
      plan_.columns()[column].name));
  int64_columns_[column] = true;
}

std::unique_ptr<PhysicalPlan> Lowering::Finish() {
  out_->pipeline_ = std::make_unique<ex::Pipeline>(
      *out_->input_, std::move(operators_), ex::ExecutionOptions());
  for (const NamedColumn& column : plan_.output()) {
    out_->columns_.push_back({column.name, Position(column.id)});
  }
  return std::move(out_);
}

PhysicalPlan::PhysicalPlan() = default;
PhysicalPlan::~PhysicalPlan() = default;

std::unique_ptr<PhysicalPlan> Lower(const LogicalPlan& plan) {
  Lowering lowering(plan);
  lowering.LowerNode(plan.root());
  return lowering.Finish();
}

}  // namespace perfetto::trace_processor::pipeline
