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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_JOIN_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_JOIN_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

class Lowering;
class PlanNode;

// `|> [INNER | LEFT] JOIN rel ON a = b` or `... USING (c)`. Joins each input
// row to the row of `rel` with the same key. For now the join must be on a
// single column, and `rel`'s keys must be consecutive, as a table's ids are.
//
// The node has two children: the input, then a scan of `rel`.
class Join : public PlanOperation {
 public:
  static const OperationRegistration kRegistration;

 private:
  std::optional<uint32_t> Prune(std::vector<bool>* needed) override;
  InputNeeds Needs(uint32_t input) const override;
  OutputProperties Derive(DeriveContext&) const override;
  void Lower(Lowering*, const PlanNode&) const override;
  void Serialize(PlanCodec&) override;
  static base::Status BuildPlan(Compiler*, uint32_t);
  const OperationRegistration& registration() const override;

  bool left_ = false;
  ColumnId probe_key_ = 0;
  ColumnId build_key_ = 0;
  // The columns of `rel` to output. They come after the input's columns.
  std::vector<ColumnId> outputs_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_JOIN_H_
