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

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor::pipeline {
namespace {

// Column IDs are unique within a plan, so one set serves every node: an
// operator's own outputs never appear in the sources below it.
using Needed = std::vector<bool>;

// Selects only `keep`, by position, from what `sql` returns. The columns are
// renamed by position first because a `SELECT *` can return two columns of
// the same name, which could then not be told apart by name.
SqlSource SelectPositions(const SqlSource& sql,
                          uint32_t count,
                          const std::vector<uint32_t>& keep) {
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
    selected += "c" + std::to_string(keep[i]);
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
  // A batch without columns would carry no rows, so keep one even when
  // nothing reads it: the rows still matter to what is above.
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
  auto& sql = std::get<SqlSource>(scan.source);
  sql = SelectPositions(sql, count, keep);
}

void PruneNode(LogicalPlan&, PlanNodeId, Needed&);

void PruneTreeAccumulate(LogicalPlan& plan,
                         const PlanNode& node,
                         Needed& needed) {
  // Passes every input column through and adds its aggregates, which need
  // the tree's shape and the columns they sum.
  //
  // TODO(lalitm): drop the aggregates whose output nothing reads, and the
  // fold itself when that is all of them. Not done yet because lowering also
  // validates the tree and reorders its rows as part of the fold, so removing
  // it would change errors and row order, not just columns. Splitting
  // validation out into an operator of its own lets the fold be pruned like
  // any other computed column.
  const auto& acc = node.Cast<op::TreeAccumulate>();
  needed[acc.node_column] = true;
  needed[acc.parent_column] = true;
  for (const op::TreeAccumulate::Aggregate& agg : acc.aggregates) {
    needed[agg.column] = true;
  }
  PruneNode(plan, node.children[0], needed);
}

void PruneIntervalIntersect(LogicalPlan& plan,
                            const PlanNode& node,
                            Needed& needed) {
  // Passes every operand column through and adds the region's bounds, which
  // need each operand's own bounds and the columns keying them.
  const auto& isect = node.Cast<op::IntervalIntersect>();
  for (const op::IntervalIntersect::Operand& operand : isect.operands) {
    needed[operand.ts] = true;
    needed[operand.dur] = true;
    for (ColumnId key : operand.keys) {
      needed[key] = true;
    }
  }
  for (PlanNodeId child : node.children) {
    PruneNode(plan, child, needed);
  }
}

void PruneNode(LogicalPlan& plan, PlanNodeId id, Needed& needed) {
  PlanNode& node = plan.nodes[id];
  switch (node.kind()) {
    case OpKind::kScan:
      PruneScan(node.Cast<op::Scan>(), needed);
      return;
    case OpKind::kTreeAccumulate:
      PruneTreeAccumulate(plan, node, needed);
      return;
    case OpKind::kIntervalIntersect:
      PruneIntervalIntersect(plan, node, needed);
      return;
  }
  PERFETTO_FATAL("For GCC");
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
  PruneNode(plan, plan.root, needed);
}

}  // namespace perfetto::trace_processor::pipeline
