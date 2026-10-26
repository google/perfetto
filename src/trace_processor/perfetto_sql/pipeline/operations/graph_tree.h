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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_GRAPH_TREE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_GRAPH_TREE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "src/trace_processor/core/exec/graph_tree.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

class Lowering;
class PlanNode;

// `GRAPH BFS | DFS | DOMINATOR TREE NODES rel EDGES rel FROM rel`. A source:
// the rows are the nodes reachable from the seeds, each with the id of its
// parent in the tree. The children are the nodes, the edges and the seeds.
class GraphTree : public PlanOperation {
 public:
  static const OperationRegistration kRegistration;

 private:
  // Test-only formatting; keep payload details out of the public interface.
  friend class LogicalPlanFormatter;

  // PlanOperation implementation. Dispatch goes through the base interface.
  std::unique_ptr<PlanOperation> Clone() const override;
  std::optional<uint32_t> Prune(std::vector<bool>* needed) override;
  void Lower(Lowering*, const PlanNode&) const override;
  void Serialize(PlanCodec&) override;
  static base::Status BuildPlan(Compiler*, uint32_t);
  const OperationRegistration& registration() const override;

  core::exec::GraphTreeSpec::Kind kind_ = core::exec::GraphTreeSpec::Kind::kBfs;
  ColumnId node_ = 0;
  // The node columns passed on to the rows out, in the nodes' order.
  std::vector<ColumnId> carried_;
  ColumnId source_ = 0;
  ColumnId target_ = 0;
  ColumnId seed_ = 0;
  // The parent's id, which no input owns.
  ColumnId parent_ = 0;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_GRAPH_TREE_H_
