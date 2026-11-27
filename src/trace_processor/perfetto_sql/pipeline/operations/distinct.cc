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

#include "src/trace_processor/perfetto_sql/pipeline/operations/distinct.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "src/trace_processor/core/exec/distinct.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

constexpr OperationRegistration Distinct::kRegistration{
    7, &MakeOperation<Distinct>};

std::optional<uint32_t> Distinct::Prune(std::vector<bool>* needed) {
  for (ColumnId column : columns_) {
    (*needed)[column] = true;
  }
  return std::nullopt;
}

OutputProperties Distinct::Derive(DeriveContext& c) const {
  OutputProperties out = c.input(0);
  out.DropRows();
  return out;
}

void Distinct::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerInput(node, 0);
  // Rows kept stay in their input order, so any known arrangement survives.
  c->AddOperator(
      c->arena().New<ex::Distinct>(c->QueryPositions(columns_.span())));
}

const OperationRegistration& Distinct::registration() const {
  return kRegistration;
}

void Distinct::Serialize(PlanCodec& c) {
  c.Vec(&columns_);
}

}  // namespace perfetto::trace_processor::pipeline
