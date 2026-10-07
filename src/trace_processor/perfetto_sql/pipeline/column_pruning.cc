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
#include <optional>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_flatten.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_intersect.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/tree_accumulate.h"

namespace perfetto::trace_processor::pipeline {
namespace {

std::optional<uint32_t> Prune(PlanNode& node, std::vector<bool>* needed) {
  switch (node.operation().index()) {
    case base::variant_index<PlanOperation, Scan>():
      return node.Cast<Scan>().Prune(needed);
    case base::variant_index<PlanOperation, TreeAccumulate>():
      return node.Cast<TreeAccumulate>().Prune(needed);
    case base::variant_index<PlanOperation, IntervalIntersect>():
      return node.Cast<IntervalIntersect>().Prune(needed);
    case base::variant_index<PlanOperation, IntervalFlatten>():
      return node.Cast<IntervalFlatten>().Prune(needed);
  }
  PERFETTO_FATAL("For GCC");
}

// Prunes the subtree at `id` and returns the node which should now stand in
// its place: usually `id` itself, but an operator whose work nobody uses is
// replaced by its input.
PlanNodeId PruneNode(LogicalPlan& plan,
                     PlanNodeId id,
                     std::vector<bool>& needed) {
  PlanNode& node = plan.nodes()[id];
  if (std::optional<uint32_t> child = Prune(node, &needed)) {
    return PruneNode(plan, node.children()[*child], needed);
  }
  for (auto& child : node.children()) {
    child = PruneNode(plan, child, needed);
  }
  return id;
}

}  // namespace

void PruneColumns(LogicalPlan& plan) {
  if (plan.nodes().empty()) {
    return;
  }
  // The columns something downstream uses, indexed by column ID.
  std::vector<bool> needed(plan.columns().size(), false);
  for (const NamedColumn& column : plan.output()) {
    needed[column.id] = true;
  }
  plan.SetRoot(PruneNode(plan, plan.root(), needed));
}

}  // namespace perfetto::trace_processor::pipeline
