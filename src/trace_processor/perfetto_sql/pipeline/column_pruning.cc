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

#include "src/trace_processor/perfetto_sql/pipeline/column_pruning.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor::pipeline {
namespace {

// The columns something downstream uses, indexed by column ID.
using Needed = std::vector<bool>;

void PruneScan(op::Scan& scan, const Needed& needed) {
  std::vector<uint32_t> keep;
  for (uint32_t i = 0; i < scan.columns.size(); ++i) {
    if (needed[scan.columns[i].id]) {
      keep.push_back(i);
    }
  }
  if (keep.size() == scan.columns.size()) {
    return;
  }
  // Always keep at least one column: a batch with no columns has no rows.
  if (keep.empty()) {
    keep.push_back(0);
  }
  std::vector<NamedColumn> columns;
  for (uint32_t i : keep) {
    columns.push_back(std::move(scan.columns[i]));
  }
  scan.columns = std::move(columns);

  switch (scan.source.index()) {
    case base::variant_index<op::Scan::Source, op::Scan::Dataframe>(): {
      auto& dataframe = base::unchecked_get<op::Scan::Dataframe>(scan.source);
      std::vector<std::shared_ptr<const dataframe::Column>> kept;
      for (uint32_t i : keep) {
        kept.push_back(std::move(dataframe.columns[i]));
      }
      dataframe.columns = std::move(kept);
      return;
    }
    case base::variant_index<op::Scan::Source, SqlSource>():
      // The SQL building a dataframe of the relation reads only the columns
      // the scan keeps.
      return;
    default:
      PERFETTO_FATAL("Unknown scan source");
  }
}

// Prunes the subtree at `id` and returns the node which should now stand in
// its place: usually `id` itself, but an operator whose work nobody uses is
// replaced by its input.
PlanNodeId PruneNode(LogicalPlan&, PlanNodeId, Needed&);

PlanNodeId PruneTreeAccumulate(LogicalPlan& plan,
                               PlanNodeId id,
                               Needed& needed) {
  PlanNode& node = plan.nodes[id];
  auto& aggregates = node.Cast<op::TreeAccumulate>().aggregates;
  aggregates.erase(
      std::remove_if(aggregates.begin(), aggregates.end(),
                     [&](const op::TreeAccumulate::Aggregate& agg) {
                       return !needed[agg.output];
                     }),
      aggregates.end());

  // A fold with nothing left to compute can go entirely. Checking that the
  // input is a valid tree is not something a fold promises on its own.
  if (aggregates.empty()) {
    return PruneNode(plan, node.children[0], needed);
  }

  // Otherwise it needs the tree's structure and the columns it aggregates.
  const auto& acc = node.Cast<op::TreeAccumulate>();
  needed[acc.node_column] = true;
  needed[acc.parent_column] = true;
  for (const op::TreeAccumulate::Aggregate& agg : acc.aggregates) {
    needed[agg.column] = true;
  }
  node.children[0] = PruneNode(plan, node.children[0], needed);
  return id;
}

PlanNodeId PruneIntervalIntersect(LogicalPlan& plan,
                                  PlanNodeId id,
                                  Needed& needed) {
  PlanNode& node = plan.nodes[id];
  // An intersection passes on only the operand columns used after it, but
  // reads each operand's bounds and partition columns to find the regions.
  auto& isect = node.Cast<op::IntervalIntersect>();
  for (op::IntervalIntersect::Operand& operand : isect.operands) {
    auto& carried = operand.carried;
    carried.erase(std::remove_if(carried.begin(), carried.end(),
                                 [&](ColumnId id) { return !needed[id]; }),
                  carried.end());
  }
  for (const op::IntervalIntersect::Operand& operand : isect.operands) {
    needed[operand.ts] = true;
    needed[operand.dur] = true;
    for (ColumnId key : operand.keys) {
      needed[key] = true;
    }
  }
  for (PlanNodeId& child : node.children) {
    child = PruneNode(plan, child, needed);
  }
  return id;
}

PlanNodeId PruneNode(LogicalPlan& plan, PlanNodeId id, Needed& needed) {
  PlanNode& node = plan.nodes[id];
  switch (node.op.index()) {
    case base::variant_index<Op, op::Scan>():
      PruneScan(node.Cast<op::Scan>(), needed);
      return id;
    case base::variant_index<Op, op::TreeAccumulate>():
      return PruneTreeAccumulate(plan, id, needed);
    case base::variant_index<Op, op::IntervalIntersect>():
      return PruneIntervalIntersect(plan, id, needed);
    default:
      PERFETTO_FATAL("Unknown operator");
  }
}

}  // namespace

void PruneColumns(LogicalPlan& plan) {
  if (plan.nodes.empty()) {
    return;
  }
  Needed needed(plan.columns.size(), false);
  for (const NamedColumn& column : plan.output) {
    needed[column.id] = true;
  }
  plan.root = PruneNode(plan, plan.root, needed);
}

}  // namespace perfetto::trace_processor::pipeline
