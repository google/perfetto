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

#include "src/trace_processor/perfetto_sql/pipeline/operations/compute.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/expression.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

const OperationRegistration Compute::kRegistration{
    std::numeric_limits<uint32_t>::max(), &BuildPlan,
    OperationRegistration::Encoding{9, &MakeOperation<Compute>}};

base::Status Compute::BuildPlan(Compiler*, uint32_t) {
  PERFETTO_FATAL("Compute has no syntax");
}

std::optional<uint32_t> Compute::Prune(std::vector<bool>* needed) {
  items_.erase(
      std::remove_if(items_.begin(), items_.end(),
                     [&](const Item& item) { return !(*needed)[item.output]; }),
      items_.end());
  if (items_.empty()) {
    return 0;
  }
  for (const Item& item : items_) {
    MarkExprColumns(item.expr, needed);
  }
  return std::nullopt;
}

void Compute::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerNode(node.children()[0]);
  std::vector<ex::ExprProgram> programs;
  std::vector<core::StorageType> types;
  for (const Item& item : items_) {
    core::StorageType type = core::Int64{};
    programs.push_back(LowerExpr(c, item.expr, &type));
    types.push_back(type);
  }
  c->AddOrderIndependent(std::make_unique<ex::Compute>(std::move(programs)));
  for (size_t i = 0; i < items_.size(); ++i) {
    c->DeclareColumn(items_[i].output);
    c->DeclareType(items_[i].output, types[i]);
  }
}

const OperationRegistration& Compute::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> Compute::Clone() const {
  return std::make_unique<Compute>(*this);
}

void Compute::Serialize(PlanCodec& c) {
  items_.resize(c.Count(items_.size()));
  for (Item& item : items_) {
    c.Pod(&item.output);
    SerializeExpr(c, &item.expr);
  }
}

}  // namespace perfetto::trace_processor::pipeline
