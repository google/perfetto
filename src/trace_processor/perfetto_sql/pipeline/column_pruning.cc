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

#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"

namespace perfetto::trace_processor::pipeline {
namespace {

// Prunes the subtree at `id` and returns the node which should now stand in
// its place: usually `id` itself, but an operation whose work nobody uses is
// replaced by its input.
PlanNodeId PruneNode(LogicalPlan& plan,
                     PlanNodeId id,
                     std::vector<bool>& needed) {
  PlanNode& node = plan.nodes()[id];
  if (std::optional<uint32_t> child = node.operation().Prune(&needed)) {
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
