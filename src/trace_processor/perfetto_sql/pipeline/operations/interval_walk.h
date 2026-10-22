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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_INTERVAL_WALK_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_INTERVAL_WALK_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

class Lowering;
class PlanNode;

// `|> INTERVAL WALK WITH rel PER cols TO cols`. For each input interval on
// `PER cols`, intersects it with `rel` on `PER cols` and transitively follows
// `TO cols` to the target `PER cols` lane in `rel` until reaching a leaf
// interval (where `TO cols` is null, the target lane has no covering interval,
// or a cycle/depth limit is reached).
//
// The node has two children: the input, then a scan of `rel`.
class IntervalWalk : public PlanOperation {
 public:
  static const OperationRegistration kRegistration;

 private:
  friend class LogicalPlanFormatter;

  std::unique_ptr<PlanOperation> Clone() const override;
  std::optional<uint32_t> Prune(std::vector<bool>* needed) override;
  void Lower(Lowering*, const PlanNode&) const override;
  void Serialize(PlanCodec&) override;

  static base::Status BuildPlan(Compiler*, uint32_t);

  const OperationRegistration& registration() const override;

  ColumnId out_ts_ = 0;
  ColumnId out_dur_ = 0;
  ColumnId in_ts_ = 0;
  ColumnId in_dur_ = 0;
  std::vector<ColumnId> keys_;
  std::vector<ColumnId> input_carried_;
  ColumnId edges_ts_ = 0;
  ColumnId edges_dur_ = 0;
  std::vector<ColumnId> edges_keys_;
  std::vector<ColumnId> edges_next_keys_;
  std::vector<ColumnId> edges_carried_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_INTERVAL_WALK_H_
