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

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/assert_type.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/tree_accumulate.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/exec/tree_order.h"
#include "src/trace_processor/perfetto_sql/exec/sql_scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

// Builds one pipeline of operators, including any blocking ordering stages.
class Lowering {
 public:
  explicit Lowering(const LogicalPlan& plan, const LowerEnvironment& env)
      : plan_(plan),
        env_(env),
        out_(std::make_unique<PhysicalPlan>()),
        positions_(plan.columns.size(), std::numeric_limits<uint32_t>::max()),
        int64_columns_(plan.columns.size(), false) {}

  void Lower(const Op&);
  std::unique_ptr<PhysicalPlan> Finish();

 private:
  void LowerScan(const op::Scan&);
  void LowerTreeAccumulate(const op::TreeAccumulate&);

  // Establishes the physical layout and ordering needed by a tree fold.
  void PrepareTree(const op::TreeAccumulate&);
  // Inserts an AssertType if `column` is not already flat Int64.
  void RequireInt64(ColumnId column);

  // Map logical IDs to physical batch columns.
  uint32_t Position(ColumnId id) const {
    PERFETTO_DCHECK(positions_[id] != std::numeric_limits<uint32_t>::max());
    return positions_[id];
  }
  void Define(ColumnId id) { positions_[id] = column_count_++; }

  // Inputs, borrowed for the duration of lowering.
  const LogicalPlan& plan_;
  const LowerEnvironment& env_;

  // Execution graph under construction.
  std::unique_ptr<PhysicalPlan> out_;
  std::vector<std::unique_ptr<ex::Operator>> operators_;

  // Logical ID -> physical batch position. Operators append columns as lowered.
  std::vector<uint32_t> positions_;
  uint32_t column_count_ = 0;
  // IDs already covered by an AssertType.
  std::vector<bool> int64_columns_;

  // Numbered tree columns are physical temporaries, with no logical IDs.
  struct TreeColumns {
    uint32_t node;
    uint32_t parent;
  };
  std::optional<TreeColumns> tree_columns_;
  std::optional<op::TreeDirection> tree_order_;
};

void Lowering::Lower(const Op& op) {
  if (const auto* scan = std::get_if<op::Scan>(&op)) {
    LowerScan(*scan);
  } else {
    LowerTreeAccumulate(std::get<op::TreeAccumulate>(op));
  }
}

void Lowering::LowerScan(const op::Scan& scan) {
  PERFETTO_DCHECK(!out_->input_);
  for (const NamedColumn& column : scan.columns) {
    Define(column.id);
  }
  if (const auto* sql = std::get_if<SqlSource>(&scan.source)) {
    Schema columns;
    columns.reserve(scan.columns.size());
    for (const NamedColumn& column : scan.columns) {
      columns.push_back({column.name, plan_.columns[column.id].type});
    }
    out_->input_ = std::make_unique<exec::SqlScan>(
        env_.connection, *sql, std::move(columns), env_.pool);
    return;
  }
  const auto& source = std::get<op::Scan::Dataframe>(scan.source);
  out_->input_ =
      std::make_unique<ex::DataframeScan>(source.columns, source.row_count);
}

void Lowering::RequireInt64(ColumnId column) {
  // TODO(lalitm): Replace this with numeric normalization and support Double
  // totals in tree accumulation. For dynamically typed inputs, a Double in a
  // later batch may require promoting earlier Int64 values too. Choosing one
  // numeric type for the whole column may therefore need a breaker that buffers
  // input before emitting any values. Update logical result typing alongside
  // the executor so SUM is no longer restricted to integers.
  const auto& type = plan_.columns[column].type;
  if ((type && type->Is<core::Int64>()) || int64_columns_[column]) {
    return;
  }
  operators_.push_back(std::make_unique<ex::AssertType>(
      Position(column), ex::AssertTypeTarget{core::Int64{}},
      plan_.columns[column].name));
  int64_columns_[column] = true;
}

void Lowering::PrepareTree(const op::TreeAccumulate& acc) {
  if (!tree_columns_) {
    operators_.push_back(std::make_unique<ex::TreeNumberNodes>(
        Position(acc.node_column), Position(acc.parent_column)));
    tree_columns_ = TreeColumns{column_count_++, column_count_++};
  }
  if (tree_order_ == acc.direction) {
    return;
  }
  if (acc.direction == op::TreeDirection::kUp) {
    operators_.push_back(std::make_unique<ex::TreeChildFirst>(
        tree_columns_->node, tree_columns_->parent));
  } else {
    operators_.push_back(std::make_unique<ex::TreeParentFirst>(
        tree_columns_->node, tree_columns_->parent));
  }
  tree_order_ = acc.direction;
}

void Lowering::LowerTreeAccumulate(const op::TreeAccumulate& acc) {
  PrepareTree(acc);
  for (const op::TreeAccumulate::Aggregate& agg : acc.aggregates) {
    PERFETTO_DCHECK(agg.function == op::TreeAccumulate::Function::kSum);
    RequireInt64(agg.column);
  }
  for (const op::TreeAccumulate::Aggregate& agg : acc.aggregates) {
    ex::TreeAccumulateSpec spec{tree_columns_->node, tree_columns_->parent,
                                Position(agg.column)};
    if (acc.direction == op::TreeDirection::kUp) {
      operators_.push_back(std::make_unique<ex::TreeAccumulateUp>(spec));
    } else {
      operators_.push_back(std::make_unique<ex::TreeAccumulateDown>(spec));
    }
    Define(agg.output);
  }
}

std::unique_ptr<PhysicalPlan> Lowering::Finish() {
  out_->pipeline_ =
      std::make_unique<ex::Pipeline>(*out_->input_, std::move(operators_));
  for (const NamedColumn& column : plan_.output) {
    out_->columns_.push_back({column.name, Position(column.id)});
  }
  return std::move(out_);
}

PhysicalPlan::PhysicalPlan() = default;
PhysicalPlan::~PhysicalPlan() = default;

std::unique_ptr<PhysicalPlan> Lower(const LogicalPlan& plan,
                                    const LowerEnvironment& env) {
  Lowering lowering(plan, env);
  for (const Op& op : plan.ops) {
    lowering.Lower(op);
  }
  return lowering.Finish();
}

}  // namespace perfetto::trace_processor::pipeline
