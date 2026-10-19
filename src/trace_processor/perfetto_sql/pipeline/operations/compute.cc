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
    OperationRegistration::Encoding{9, false, &DecodePlan}};

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
    programs.push_back(LowerExpr(c, item.expr, c->context().pool, &type));
    types.push_back(type);
  }
  c->AddRowLocal(std::make_unique<ex::Compute>(std::move(programs)));
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

void Compute::Write(PlanWriter* c,
                    const PlanNode&,
                    Available* available) const {
  c->writer().Size(items_.size());
  for (const Item& item : items_) {
    const PlanColumn& column = c->plan().columns()[item.output];
    c->writer().Str(column.name);
    WriteType(&c->writer(), column.type);
    WriteExpr(c, item.expr, *available);
  }
  for (const Item& item : items_) {
    available->push_back(item.output);
  }
}

void Compute::DecodePlan(PlanReader* c, Available* available) {
  std::vector<Item> items(c->reader().Count());
  if (items.empty()) {
    c->reader().Fail();
  }
  for (Item& item : items) {
    std::string name = c->reader().Str();
    PlanType type = ReadType(&c->reader());
    item.expr = ReadExpr(c, *available);
    item.output = c->AddColumn(std::move(name), type);
  }
  for (const Item& item : items) {
    available->push_back(item.output);
  }
  c->AddNode(Compute(std::move(items)), {c->plan().root()});
}

}  // namespace perfetto::trace_processor::pipeline
