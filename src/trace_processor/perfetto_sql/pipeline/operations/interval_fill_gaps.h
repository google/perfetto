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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_INTERVAL_FILL_GAPS_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_INTERVAL_FILL_GAPS_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

class Lowering;
class PlanNode;

// `|> INTERVAL FILL GAPS WITH rel [PER cols]`. Every row passes through, and
// each span of `rel` its lane does not cover becomes a row taking `rel`'s
// values by column name. The node reads two children: the input, then a scan
// of the background.
class IntervalFillGaps : public PlanOperation {
 public:
  static const OperationRegistration kRegistration;

 private:
  // A column of the result: the input column an input row shows, and the
  // background column a filler shows. A filler's ts and dur are its own.
  struct Output {
    ColumnId column = 0;
    std::optional<ColumnId> input;
    std::optional<ColumnId> background;
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
  ColumnId background_ts_ = 0;
  ColumnId background_dur_ = 0;
  // Either the background's counterparts of keys_, in order, or empty.
  std::vector<ColumnId> background_keys_;
  std::vector<Output> outputs_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_INTERVAL_FILL_GAPS_H_
