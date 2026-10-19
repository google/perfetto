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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_COMPUTE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_COMPUTE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/perfetto_sql/pipeline/expression.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operation_registry.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

// Adds columns computed from the row: what SELECT, EXTEND and SET add for an
// item which is more than a column. Those stages add it, so it has no syntax
// of its own.
class Compute : public PlanOperation {
 public:
  static const OperationRegistration kRegistration;

  struct Item {
    ColumnId output;
    Expr expr;
  };

  explicit Compute(std::vector<Item> items) : items_(std::move(items)) {}

 private:
  friend class LogicalPlanFormatter;

  std::unique_ptr<PlanOperation> Clone() const override;
  std::optional<uint32_t> Prune(std::vector<bool>* needed) override;
  void Lower(Lowering*, const PlanNode&) const override;
  void Write(PlanWriter*, const PlanNode&, Available*) const override;

  static base::Status BuildPlan(Compiler*, uint32_t);
  static void DecodePlan(PlanReader*, Available*);

  const OperationRegistration& registration() const override;

  std::vector<Item> items_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_COMPUTE_H_
