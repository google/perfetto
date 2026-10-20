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

#include "src/trace_processor/perfetto_sql/pipeline/filter_passing.h"

#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/where.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {
namespace {

// Filters the rows of the node at `id` by `predicates`, returning the node
// which should now stand in its place.
PlanNodeId AddWhere(LogicalPlan& plan,
                    PlanNodeId id,
                    std::vector<WherePredicate> predicates) {
  if (plan.nodes()[id].Is<Where>()) {
    auto& where = static_cast<Where&>(plan.nodes()[id].operation());
    where.predicates().insert(where.predicates().end(),
                              std::make_move_iterator(predicates.begin()),
                              std::make_move_iterator(predicates.end()));
    return id;
  }
  // Adding a node makes it the root, which this one is not.
  PlanNodeId root = plan.root();
  PlanNodeId where = plan.AddNode(Where(std::move(predicates)), {id});
  plan.SetRoot(root);
  return where;
}

// Passes down the WHEREs in the subtree at `id`, returning the node which
// should now stand in its place.
PlanNodeId Visit(LogicalPlan& plan, PlanNodeId id) {
  if (plan.nodes()[id].Is<Where>()) {
    // Operations are owned by their node, so this outlives nodes being added.
    auto& where = static_cast<Where&>(plan.nodes()[id].operation());
    PlanNodeId child = plan.nodes()[id].children()[0];
    std::vector<std::vector<WherePredicate>> passed =
        plan.nodes()[child].operation().PassFilters(plan.nodes()[child],
                                                    &where.predicates());
    for (size_t k = 0; k < passed.size(); ++k) {
      if (passed[k].empty()) {
        continue;
      }
      PlanNodeId input = plan.nodes()[child].children()[k];
      PlanNodeId filtered = AddWhere(plan, input, std::move(passed[k]));
      plan.nodes()[child].children()[k] = filtered;
    }
    child = Visit(plan, child);
    plan.nodes()[id].children()[0] = child;
    return where.predicates().empty() ? child : id;
  }
  for (size_t i = 0; i < plan.nodes()[id].children().size(); ++i) {
    PlanNodeId child = Visit(plan, plan.nodes()[id].children()[i]);
    plan.nodes()[id].children()[i] = child;
  }
  return id;
}

}  // namespace

void PassFiltersThrough(LogicalPlan& plan) {
  if (plan.nodes().empty()) {
    return;
  }
  plan.SetRoot(Visit(plan, plan.root()));
}

}  // namespace perfetto::trace_processor::pipeline
