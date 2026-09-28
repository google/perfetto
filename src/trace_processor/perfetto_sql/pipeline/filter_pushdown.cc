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

#include "src/trace_processor/perfetto_sql/pipeline/filter_pushdown.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"

namespace perfetto::trace_processor::pipeline {
namespace {

using Conditions = std::vector<op::FilterCondition>;

// The position of `id` in `ids`, if it is there.
std::optional<size_t> Find(const std::vector<ColumnId>& ids, ColumnId id) {
  auto it = std::find(ids.begin(), ids.end(), id);
  return it == ids.end() ? std::nullopt
                         : std::make_optional<size_t>(
                               static_cast<size_t>(it - ids.begin()));
}

// Moves `conditions` into the subtree at `id` as far as they go. Returns
// those which have to stay above it.
Conditions Push(LogicalPlan& plan, PlanNodeId id, Conditions conditions) {
  PlanNode& node = plan.nodes[id];
  switch (node.op.index()) {
    case base::variant_index<Op, op::Scan>(): {
      auto& filters = node.Cast<op::Scan>().filters;
      filters.insert(filters.end(), conditions.begin(), conditions.end());
      return {};
    }
    case base::variant_index<Op, op::Filter>(): {
      // Its own conditions went as far as they could, so these stop here too.
      auto& own = node.Cast<op::Filter>().conditions;
      own.insert(own.end(), conditions.begin(), conditions.end());
      return {};
    }
    case base::variant_index<Op, op::TreeAccumulate>():
      return conditions;
    case base::variant_index<Op, op::IntervalIntersect>(): {
      const auto& isect = node.Cast<op::IntervalIntersect>();
      std::vector<Conditions> per_operand(isect.operands.size());
      Conditions kept;
      for (op::FilterCondition& condition : conditions) {
        bool pushed = false;
        for (size_t k = 0; k < isect.operands.size() && !pushed; ++k) {
          const op::IntervalIntersect::Operand& operand = isect.operands[k];
          if (std::optional<size_t> key =
                  Find(operand.keys, condition.column)) {
            for (size_t j = 0; j < isect.operands.size(); ++j) {
              op::FilterCondition copy = condition;
              copy.column = isect.operands[j].keys[*key];
              per_operand[j].push_back(std::move(copy));
            }
            pushed = true;
          } else if (Find(operand.carried, condition.column)) {
            per_operand[k].push_back(condition);
            pushed = true;
          }
        }
        // The region's own bounds belong to no operand.
        if (!pushed) {
          kept.push_back(std::move(condition));
        }
      }
      for (size_t k = 0; k < per_operand.size(); ++k) {
        if (!per_operand[k].empty()) {
          // Operands are scans, which take every condition.
          Conditions rest =
              Push(plan, plan.nodes[id].children[k], std::move(per_operand[k]));
          PERFETTO_DCHECK(rest.empty());
        }
      }
      return kept;
    }
    default:
      PERFETTO_FATAL("Unknown operator");
  }
}

// Pushes down the filters in the subtree at `id`, returning the node which
// should now stand in its place.
PlanNodeId Visit(LogicalPlan& plan, PlanNodeId id) {
  for (size_t i = 0; i < plan.nodes[id].children.size(); ++i) {
    PlanNodeId child = Visit(plan, plan.nodes[id].children[i]);
    plan.nodes[id].children[i] = child;
  }
  PlanNode& node = plan.nodes[id];
  if (!node.Is<op::Filter>()) {
    return id;
  }
  PlanNodeId child = node.children[0];
  Conditions rest =
      Push(plan, child, std::move(node.Cast<op::Filter>().conditions));
  auto& filter = plan.nodes[id].Cast<op::Filter>();
  filter.conditions = std::move(rest);
  return filter.conditions.empty() ? child : id;
}

}  // namespace

void PushDownFilters(LogicalPlan& plan) {
  if (plan.nodes.empty()) {
    return;
  }
  plan.root = Visit(plan, plan.root);
}

}  // namespace perfetto::trace_processor::pipeline
