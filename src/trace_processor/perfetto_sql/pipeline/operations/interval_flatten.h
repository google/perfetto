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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_INTERVAL_FLATTEN_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_INTERVAL_FLATTEN_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

class Lowering;
class PlanNode;

// `|> INTERVAL FLATTEN [PER cols] AGGREGATE agg AS name, ...`. Cuts the rows
// at every start and end into disjoint segments, each collapsed into one row:
// its bounds, the keys, then one column per aggregate.
class IntervalFlatten : public PlanOperation {
 public:
  static const OperationRegistration kRegistration;

 private:
  enum class Function : uint8_t { kCount, kSum };
  struct Aggregate {
    Function function = Function::kCount;
    // Unused by COUNT(*).
    ColumnId column = 0;
    ColumnId output = 0;
  };

  // Test-only formatting; keep payload details out of the public interface.
  friend class LogicalPlanFormatter;

  // PlanOperation implementation. Dispatch goes through the base interface.
  std::unique_ptr<PlanOperation> Clone() const override;
  std::optional<uint32_t> Prune(std::vector<bool>* needed) override;
  void Lower(Lowering*, const PlanNode&) const override;
  void Write(PlanWriter*, const PlanNode&, Available*) const override;

  static base::Status BuildPlan(Compiler*, uint32_t);
  static void DecodePlan(PlanReader*, Available*);

  const OperationRegistration& registration() const override;

  ColumnId ts_ = 0;
  ColumnId dur_ = 0;
  std::vector<ColumnId> keys_;
  std::vector<Aggregate> aggregates_;
  ColumnId out_ts_ = 0;
  ColumnId out_dur_ = 0;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_INTERVAL_FLATTEN_H_
