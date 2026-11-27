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

#include "src/trace_processor/perfetto_sql/pipeline/operations/limit.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "src/trace_processor/core/exec/limit.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;
namespace {

// Batches count rows in uint32_t, so more than that many is as good as all.
uint32_t Clamp(int64_t value) {
  return static_cast<uint32_t>(
      std::min<int64_t>(value, std::numeric_limits<uint32_t>::max()));
}

}  // namespace

constexpr OperationRegistration Limit::kRegistration{11, &MakeOperation<Limit>};

std::optional<uint32_t> Limit::Prune(std::vector<bool>*) {
  return std::nullopt;
}

OutputProperties Limit::Derive(DeriveContext& c) const {
  OutputProperties out = c.input(0);
  if (count_ >= 0 && out.open_sort) {
    // The sort just before only needs to output the rows read here.
    c.LimitSort(*out.open_sort, Clamp(int64_t{Clamp(count_)} +
                                      Clamp(std::max<int64_t>(offset_, 0))));
  }
  out.DropRows();
  return out;
}

void Limit::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerInput(node, 0);
  std::optional<uint32_t> count;
  if (count_ >= 0) {
    count = Clamp(count_);
  }
  // A sort which outputs only the rows read here leaves nothing to drop.
  if (count && offset_ <= 0 && c->QuerySortKeep() == count) {
    return;
  }
  // Rows kept stay in their input order, so any known arrangement survives.
  c->AddOperator(
      c->arena().New<ex::Limit>(count, Clamp(std::max<int64_t>(offset_, 0))));
}

const OperationRegistration& Limit::registration() const {
  return kRegistration;
}

void Limit::Serialize(PlanCodec& c) {
  c.Pod(&count_);
  c.Pod(&offset_);
}

}  // namespace perfetto::trace_processor::pipeline
