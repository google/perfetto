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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_TREE_FROM_INTERVALS_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_TREE_FROM_INTERVALS_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operation_registry.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

// `|> TREE FROM INTERVALS ON <expr>`: connects each `child` interval to its
// overlapping `parent` interval matching `<expr>`, cuts child intervals at
// parent boundaries (transitively), and appends `id` and `parent_id` forming
// a forest.
class TreeFromIntervals : public PlanOperation {
 public:
  static const OperationRegistration kRegistration;

 private:
  // Test-only formatting; keep payload details out of the public interface.
  friend class LogicalPlanFormatter;

  // PlanOperation implementation. Dispatch goes through the base interface.
  std::unique_ptr<PlanOperation> Clone() const override;
  std::optional<uint32_t> Prune(std::vector<bool>* needed) override;
  void Lower(Lowering* c, const PlanNode& node) const override;
  void Serialize(PlanCodec& c) override;
  static base::Status BuildPlan(Compiler* c, uint32_t stage);
  const OperationRegistration& registration() const override;

  ColumnId in_ts_ = 0;
  ColumnId in_dur_ = 0;
  std::vector<ColumnId> keys_;
  std::vector<ColumnId> next_keys_;
  ColumnId out_ts_ = 0;
  ColumnId out_dur_ = 0;
  ColumnId out_id_ = 0;
  ColumnId out_parent_id_ = 0;
  // The input columns passed on to the rows out, in the input's order.
  std::vector<ColumnId> carried_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_TREE_FROM_INTERVALS_H_
