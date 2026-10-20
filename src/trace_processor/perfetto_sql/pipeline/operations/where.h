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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_WHERE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_WHERE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operation_registry.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

class Lowering;
class PlanNode;

// `|> WHERE condition`. The condition must be a conjunction (AND) of
// `column OP literal` comparisons; anything else is unsupported. Constraints
// SQLite passes on a pipeline's output are pushed into its plan as one too
// (see WherePredicate).
class Where : public PlanOperation {
 public:
  static const OperationRegistration kRegistration;

  Where() = default;
  explicit Where(std::vector<WherePredicate> predicates)
      : predicates_(std::move(predicates)) {}

  // Keeps only the rows of `plan` every one of `predicates`, on its output,
  // holds for: in its last stage if that is a WHERE already, or a new one.
  static void AddToPlan(LogicalPlan* plan,
                        std::vector<WherePredicate> predicates);

  std::vector<WherePredicate>& predicates() { return predicates_; }
  const std::vector<WherePredicate>& predicates() const { return predicates_; }

  // Writes `predicates`, on the columns `available`, as a WHERE's payload is
  // written, and reads them back: for an operation which writes a WHERE on
  // one of its inputs with it.
  static void WritePredicates(PlanWriter*,
                              const std::vector<WherePredicate>& predicates,
                              const Available& available);
  static std::vector<WherePredicate> ReadPredicates(PlanReader*,
                                                    const Available& available);

  // Adds the filter value `p` compares a column of storage type `type` with,
  // and returns its index: a literal cast now, or a parameter cast as each
  // run starts. Unused by IS [NOT] NULL.
  static uint32_t LowerValue(Lowering* c,
                             const WherePredicate& p,
                             core::StorageType type);

 private:
  // Test-only formatting; keep payload details out of the public interface.
  friend class LogicalPlanFormatter;

  // PlanOperation implementation. Dispatch goes through the base interface.
  std::unique_ptr<PlanOperation> Clone() const override;
  std::optional<uint32_t> Prune(std::vector<bool>* needed) override;
  void Lower(Lowering*, const PlanNode&) const override;
  core::RowEstimate Estimate(PlanEstimator*, const PlanNode&) const override;
  void Write(PlanWriter*, const PlanNode&, Available*) const override;

  static base::Status BuildPlan(Compiler*, uint32_t);
  static void DecodePlan(PlanReader*, Available*);

  const OperationRegistration& registration() const override;

  std::vector<WherePredicate> predicates_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_WHERE_H_
