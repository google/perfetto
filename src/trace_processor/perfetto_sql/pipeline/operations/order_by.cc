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

#include "src/trace_processor/perfetto_sql/pipeline/operations/order_by.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {

constexpr OperationRegistration OrderBy::kRegistration{4,
                                                       &MakeOperation<OrderBy>};

void OrderBy::AddToPlan(LogicalPlan* plan, const std::vector<SortKey>& keys) {
  OrderBy order;
  order.keys_ = plan->arena().CopySpan<SortKey>(keys);
  plan->AddNode(order, {plan->root()});
}

std::optional<uint32_t> OrderBy::Prune(std::vector<bool>* needed) {
  for (const SortKey& key : keys_) {
    (*needed)[key.column] = true;
  }
  return std::nullopt;
}

InputNeeds OrderBy::Needs(uint32_t) const {
  InputNeeds needs;
  needs.arranged =
      OutputProperties::Arrangement{{}, {keys_.begin(), keys_.end()}};
  return needs;
}

OutputProperties OrderBy::Derive(DeriveContext& c) const {
  return c.input(0);
}

void OrderBy::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerInput(node, 0);
}

const OperationRegistration& OrderBy::registration() const {
  return kRegistration;
}

void OrderBy::Serialize(PlanCodec& c) {
  c.Vec(&keys_);
}

}  // namespace perfetto::trace_processor::pipeline
