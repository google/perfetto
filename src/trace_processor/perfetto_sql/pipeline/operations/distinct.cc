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
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/exec/distinct.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/cost_estimation.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

const OperationRegistration Distinct::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_PIPE_DISTINCT, &BuildPlan,
    OperationRegistration::Encoding{7, false, &DecodePlan}};

base::Status Distinct::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("DISTINCT");
  if (c->row().size() > ex::Distinct::kMaxColumns) {
    return c->Unsupported(stage, "a row of more than 64 columns");
  }
  Distinct distinct;
  for (const Compiler::RowColumn& column : c->row()) {
    distinct.columns_.push_back(column.column.id);
  }
  c->AddNode(std::move(distinct), {c->plan().root()});
  return base::OkStatus();
}

std::optional<uint32_t> Distinct::Prune(std::vector<bool>* needed) {
  for (ColumnId column : columns_) {
    (*needed)[column] = true;
  }
  return std::nullopt;
}

core::RowEstimate Distinct::Estimate(PlanEstimator* e,
                                     const PlanNode& node) const {
  // Each row's columns are looked up among those seen. Nothing is known of
  // how many repeat, so every row is assumed kept.
  core::RowEstimate in = e->EstimateNode(node.children()[0]);
  e->AddCost(PlanEstimator::kHashCostPerRow * in.estimated);
  return in;
}

void Distinct::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerNode(node.children()[0]);
  std::vector<uint32_t> positions;
  positions.reserve(columns_.size());
  for (ColumnId column : columns_) {
    positions.push_back(c->QueryPosition(column));
  }
  // Rows kept stay in their input order, so any known arrangement survives.
  c->AddOperator(std::make_unique<ex::Distinct>(std::move(positions)));
  c->DeclareSmallBatches();
}

const OperationRegistration& Distinct::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> Distinct::Clone() const {
  return std::make_unique<Distinct>(*this);
}

void Distinct::Write(PlanWriter* c,
                     const PlanNode&,
                     Available* available) const {
  c->writer().Size(columns_.size());
  for (ColumnId column : columns_) {
    c->writer().Position(*available, column);
  }
}

void Distinct::DecodePlan(PlanReader* c, Available* available) {
  Distinct distinct;
  distinct.columns_.resize(c->reader().Count());
  if (distinct.columns_.empty() ||
      distinct.columns_.size() > ex::Distinct::kMaxColumns) {
    c->reader().Fail();
  }
  for (ColumnId& column : distinct.columns_) {
    column = c->reader().Position(*available);
  }
  c->AddNode(std::move(distinct), {c->plan().root()});
}

}  // namespace perfetto::trace_processor::pipeline
