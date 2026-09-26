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

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor::pipeline {
namespace {

// The columns something downstream uses, indexed by column ID.
using Needed = std::vector<bool>;

// Narrows `sql` down to the columns at the positions in `keep`, which keep
// their names from `kept`. We pick by position rather than name because a
// query can return two columns with the same name. The scan checks the names
// it reads back, so each column is given its name again.
SqlSource SelectPositions(const SqlSource& sql,
                          uint32_t count,
                          const std::vector<uint32_t>& keep,
                          const std::vector<NamedColumn>& kept) {
  std::string names;
  for (uint32_t i = 0; i < count; ++i) {
    if (i) {
      names += ", ";
    }
    names += "c" + std::to_string(i);
  }
  std::string selected;
  for (uint32_t i = 0; i < keep.size(); ++i) {
    if (i) {
      selected += ", ";
    }
    selected += "c" + std::to_string(keep[i]) + " AS \"" +
                base::ReplaceAll(kept[i].name, "\"", "\"\"") + "\"";
  }
  return sql.RewriteAllIgnoreExisting(
      SqlSource::FromTraceProcessorImplementation(
          "WITH __pipeline_source(" + names + ") AS (" + sql.sql() +
          ") SELECT " + selected + " FROM __pipeline_source"));
}

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
  auto count = static_cast<uint32_t>(scan.columns.size());
  std::vector<NamedColumn> columns;
  for (uint32_t i : keep) {
    columns.push_back(std::move(scan.columns[i]));
  }
  scan.columns = std::move(columns);

  if (auto* dataframe = std::get_if<op::Scan::Dataframe>(&scan.source)) {
    std::vector<std::shared_ptr<const dataframe::Column>> kept;
    for (uint32_t i : keep) {
      kept.push_back(std::move(dataframe->columns[i]));
    }
    dataframe->columns = std::move(kept);
    return;
  }
  auto& sql = base::unchecked_get<SqlSource>(scan.source);
  sql = SelectPositions(sql, count, keep, scan.columns);
}

// Prunes the subtree at `id` and returns the node which should now stand in
// its place: usually `id` itself, but an operator whose work nobody uses is
// replaced by its input.
PlanNodeId PruneNode(LogicalPlan& plan, PlanNodeId id, Needed& needed) {
  PlanNode& node = plan.nodes[id];
  if (auto* scan = std::get_if<op::Scan>(&node.op)) {
    PruneScan(*scan, needed);
    return id;
  }
  if (auto* acc = std::get_if<op::TreeAccumulate>(&node.op)) {
    auto& aggregates = acc->aggregates;
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
    needed[acc->node_column] = true;
    needed[acc->parent_column] = true;
    for (const op::TreeAccumulate::Aggregate& agg : aggregates) {
      needed[agg.column] = true;
    }
    node.children[0] = PruneNode(plan, node.children[0], needed);
    return id;
  }
  // An intersection passes on only the operand columns used after it, but
  // reads each operand's bounds and partition columns to find the regions.
  auto& isect = base::unchecked_get<op::IntervalIntersect>(node.op);
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
