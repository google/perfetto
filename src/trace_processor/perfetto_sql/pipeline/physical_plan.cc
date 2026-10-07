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
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/core/common/sort_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/assert_type.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/group_by.h"
#include "src/trace_processor/core/exec/interval_flatten.h"
#include "src/trace_processor/core/exec/interval_intersect.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/sort.h"
#include "src/trace_processor/core/exec/tree_accumulate.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/exec/tree_order.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

// Builds one pipeline of operators, including any blocking ordering stages.
class Lowering {
 public:
  explicit Lowering(const LogicalPlan& plan)
      : plan_(plan),
        out_(std::make_unique<PhysicalPlan>()),
        positions_(plan.columns().size(), std::numeric_limits<uint32_t>::max()),
        int64_columns_(plan.columns().size(), false) {}

  void LowerNode(PlanNodeId);
  std::unique_ptr<PhysicalPlan> Finish();

 private:
  // Numbered tree columns are physical temporaries, with no logical IDs.
  struct TreeColumns {
    uint32_t node;
    uint32_t parent;
  };

  // The known order of the rows: grouped by `grouped_by` (numbered in
  // `group_column`), with `ascending` ascending within each group.
  struct Order {
    std::vector<ColumnId> grouped_by;
    uint32_t group_column = 0;
    std::vector<ColumnId> ascending;
  };

  void LowerScan(const Scan&);
  void LowerIntervalIntersect(const IntervalIntersect&,
                              const std::vector<PlanNodeId>& children);

  std::unique_ptr<ex::Source> MakeSource(const Scan&) const;
  void LowerTreeAccumulate(const TreeAccumulate&);
  void LowerIntervalFlatten(const IntervalFlatten&);

  // Establishes the physical layout and ordering needed by a tree fold.
  void PrepareTree(const TreeAccumulate&);
  // Groups rows by `keys`, ordered by `ascending` within each group, adding
  // only the Sort and GroupBy not already established.
  void PrepareGroups(const std::vector<ColumnId>& keys, ColumnId ascending);
  // Appends to `into` an AssertType on the column at `position` if `column`
  // is not already flat Int64. The position is the column's in the pipeline
  // `into` belongs to, which is not this one for an intersection's operand.
  void RequireInt64(ColumnId column,
                    uint32_t position,
                    std::vector<ex::Pipeline::Step>& into);

  // Map logical IDs to physical batch columns.
  uint32_t Position(ColumnId id) const {
    PERFETTO_DCHECK(positions_[id] != std::numeric_limits<uint32_t>::max());
    return positions_[id];
  }
  void Define(ColumnId id) { positions_[id] = column_count_++; }

  // Borrowed for the duration of lowering.
  const LogicalPlan& plan_;

  // Execution graph under construction.
  std::unique_ptr<PhysicalPlan> out_;
  std::vector<ex::Pipeline::Step> operators_;

  // Logical ID -> physical batch position. Operators append columns as lowered.
  std::vector<uint32_t> positions_;
  uint32_t column_count_ = 0;
  // IDs already covered by an AssertType.
  std::vector<bool> int64_columns_;

  std::optional<TreeColumns> tree_columns_;
  std::optional<TreeDirection> tree_order_;

  Order order_;
};

void Lowering::LowerNode(PlanNodeId id) {
  const PlanNode& node = plan_.nodes()[id];
  switch (node.operation().index()) {
    case base::variant_index<PlanOperation, Scan>():
      LowerScan(node.Cast<Scan>());
      return;
    case base::variant_index<PlanOperation, TreeAccumulate>():
      LowerNode(node.children()[0]);
      LowerTreeAccumulate(node.Cast<TreeAccumulate>());
      return;
    case base::variant_index<PlanOperation, IntervalIntersect>():
      // Each operand runs as its own pipeline, so the intersection lowers
      // its children itself.
      LowerIntervalIntersect(node.Cast<IntervalIntersect>(), node.children());
      return;
    case base::variant_index<PlanOperation, IntervalFlatten>():
      LowerNode(node.children()[0]);
      LowerIntervalFlatten(node.Cast<IntervalFlatten>());
      return;
    default:
      PERFETTO_FATAL("Unknown operator");
  }
}

std::unique_ptr<ex::Source> Lowering::MakeSource(const Scan& scan) const {
  switch (scan.source().index()) {
    case base::variant_index<Scan::Source, Scan::Dataframe>(): {
      const auto& source = base::unchecked_get<Scan::Dataframe>(scan.source());
      return std::make_unique<ex::DataframeScan>(source.columns,
                                                 source.row_count);
    }
    default:
      // SQL is moved out into dataframe arguments, and those are bound to
      // dataframes, before a plan is run.
      PERFETTO_FATAL("Unknown scan source");
  }
}

void Lowering::LowerScan(const Scan& scan) {
  PERFETTO_DCHECK(!out_->input_);
  for (const NamedColumn& column : scan.columns()) {
    Define(column.id);
  }
  out_->input_ = MakeSource(scan);
  // A dataframe is read in order, so a column it keeps sorted ascends.
  const auto& dataframe = base::unchecked_get<Scan::Dataframe>(scan.source());
  for (uint32_t i = 0; i < scan.columns().size(); ++i) {
    if (!dataframe.columns[i]->sort_state.Is<core::Unsorted>()) {
      order_.ascending.push_back(scan.columns()[i].id);
    }
  }
}

void Lowering::LowerIntervalIntersect(const IntervalIntersect& isect,
                                      const std::vector<PlanNodeId>& children) {
  PERFETTO_DCHECK(!out_->input_);
  PERFETTO_DCHECK(isect.operands().size() == children.size());
  // The region's bounds come first, then each operand's carried columns in
  // turn.
  Define(isect.ts());
  Define(isect.dur());

  std::vector<ex::IntervalIntersectOperand> operands;
  for (uint32_t i = 0; i < isect.operands().size(); i++) {
    const IntervalIntersect::Operand& operand = isect.operands()[i];
    // A node may read any child, but an operand is read as a scan of its own,
    // which is what lets it become a pipeline separate from this one.
    const PlanNode& child = plan_.nodes()[children[i]];
    PERFETTO_DCHECK(child.Is<Scan>());
    const auto& scan = child.Cast<Scan>();
    // Roles are named by plan-wide ID, while the operator reads batch
    // positions, so each is resolved against the operand's own column order.
    auto position = [&](ColumnId id) {
      uint32_t at = 0;
      while (scan.columns()[at].id != id) {
        ++at;
      }
      return at;
    };
    // An operand is read through a pipeline of its own, which widens its
    // bounds to Int64 where they are not already.
    std::vector<ex::Pipeline::Step> widen;
    ex::IntervalIntersectOperand lowered;
    lowered.ts_column = position(operand.ts);
    lowered.dur_column = position(operand.dur);
    for (ColumnId key : operand.keys) {
      lowered.key_columns.push_back(position(key));
    }
    for (ColumnId id : operand.carried) {
      lowered.retained_columns.push_back(position(id));
    }
    RequireInt64(operand.ts, lowered.ts_column, widen);
    RequireInt64(operand.dur, lowered.dur_column, widen);
    out_->operand_inputs_.push_back(MakeSource(scan));
    out_->operand_pipelines_.push_back(std::make_unique<ex::Pipeline>(
        *out_->operand_inputs_.back(), std::move(widen),
        ex::ExecutionOptions()));
    lowered.source = out_->operand_pipelines_.back().get();
    operands.push_back(std::move(lowered));

    for (ColumnId id : operand.carried) {
      Define(id);
    }
  }
  out_->input_ = std::make_unique<ex::IntervalIntersect>(std::move(operands));
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

void Lowering::LowerIntervalFlatten(const IntervalFlatten& flatten) {
  RequireInt64(flatten.ts(), Position(flatten.ts()), operators_);
  RequireInt64(flatten.dur(), Position(flatten.dur()), operators_);
  PrepareGroups(flatten.keys(), flatten.ts());
  ex::IntervalFlattenSpec spec;
  spec.ts_column = Position(flatten.ts());
  spec.dur_column = Position(flatten.dur());
  for (ColumnId key : flatten.keys()) {
    spec.key_columns.push_back(Position(key));
  }
  spec.group_column = order_.group_column;
  for (const IntervalFlatten::Aggregate& agg : flatten.aggregates()) {
    ex::IntervalFlattenSpec::Aggregate lowered;
    switch (agg.function) {
      case IntervalFlatten::Function::kCount:
        lowered.function = ex::IntervalFlattenSpec::Function::kCount;
        break;
      case IntervalFlatten::Function::kSum:
        RequireInt64(agg.column, Position(agg.column), operators_);
        lowered.function = ex::IntervalFlattenSpec::Function::kSum;
        lowered.column = Position(agg.column);
        break;
    }
    spec.aggregates.push_back(lowered);
  }
  operators_.push_back(std::make_unique<ex::IntervalFlatten>(std::move(spec)));

  // Its rows are the segments, laid out afresh.
  column_count_ = 0;
  Define(flatten.out_ts());
  Define(flatten.out_dur());
  for (ColumnId key : flatten.keys()) {
    Define(key);
  }
  for (const IntervalFlatten::Aggregate& agg : flatten.aggregates()) {
    Define(agg.output);
  }
  order_ = Order();
  order_.grouped_by = flatten.keys();
  order_.group_column = column_count_++;
  order_.ascending.push_back(flatten.out_ts());
  tree_columns_.reset();
  tree_order_.reset();
}

void Lowering::RequireInt64(ColumnId column,
                            uint32_t position,
                            std::vector<ex::Pipeline::Step>& into) {
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
  into.push_back(std::make_unique<ex::AssertType>(
      position, ex::AssertTypeTarget{core::Int64{}},
      plan_.columns()[column].name));
  int64_columns_[column] = true;
}

void Lowering::PrepareTree(const TreeAccumulate& acc) {
  if (!tree_columns_) {
    operators_.push_back(std::make_unique<ex::TreeNumberNodes>(
        Position(acc.node_column()), Position(acc.parent_column())));
    tree_columns_ = TreeColumns{column_count_++, column_count_++};
  }
  if (tree_order_ == acc.direction()) {
    return;
  }
  order_ = Order();
  if (acc.direction() == TreeDirection::kUp) {
    operators_.push_back(std::make_unique<ex::TreeChildFirst>(
        tree_columns_->node, tree_columns_->parent));
  } else {
    operators_.push_back(std::make_unique<ex::TreeParentFirst>(
        tree_columns_->node, tree_columns_->parent));
  }
  tree_order_ = acc.direction();
}

void Lowering::LowerTreeAccumulate(const TreeAccumulate& acc) {
  PrepareTree(acc);
  for (const TreeAccumulate::Aggregate& agg : acc.aggregates()) {
    PERFETTO_DCHECK(agg.function == TreeAccumulate::Function::kSum);
    RequireInt64(agg.column, Position(agg.column), operators_);
  }
  for (const TreeAccumulate::Aggregate& agg : acc.aggregates()) {
    ex::TreeAccumulateSpec spec{tree_columns_->node, tree_columns_->parent,
                                Position(agg.column)};
    if (acc.direction() == TreeDirection::kUp) {
      operators_.push_back(std::make_unique<ex::TreeAccumulateUp>(spec));
    } else {
      operators_.push_back(std::make_unique<ex::TreeAccumulateDown>(spec));
    }
    Define(agg.output);
  }
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
