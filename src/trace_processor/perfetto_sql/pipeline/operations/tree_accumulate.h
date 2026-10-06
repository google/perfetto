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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_TREE_ACCUMULATE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_TREE_ACCUMULATE_H_

#include <cstdint>
#include <vector>

#include "src/trace_processor/perfetto_sql/pipeline/operation_registry.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

enum class TreeDirection : uint8_t { kUp, kDown };

// `|> TREE ACCUMULATE UP | DOWN agg AS name, ...`. Appends one column per
// aggregate.
class TreeAccumulate {
 public:
  // Payload types remain public while central plan passes use them.
  enum class Function : uint8_t { kSum };
  struct Aggregate {
    Function function = Function::kSum;
    ColumnId column = 0;
    ColumnId output = 0;
  };

  static const OperationRegistration kRegistration;

  // Temporary accessors for plan passes not yet moved into this class.
  // Removed in the final migration commit; test formatting uses friend access.
  TreeDirection& direction() { return direction_; }
  const TreeDirection& direction() const { return direction_; }
  ColumnId& node_column() { return node_column_; }
  const ColumnId& node_column() const { return node_column_; }
  ColumnId& parent_column() { return parent_column_; }
  const ColumnId& parent_column() const { return parent_column_; }
  std::vector<Aggregate>& aggregates() { return aggregates_; }
  const std::vector<Aggregate>& aggregates() const { return aggregates_; }

 private:
  // Test-only formatting; keep payload details out of the public interface.
  friend class LogicalPlanFormatter;

  static base::Status BuildPlan(Compiler*, uint32_t);

  TreeDirection direction_ = TreeDirection::kUp;
  // The logical id/parent_id columns describing the input tree.
  ColumnId node_column_ = 0;
  ColumnId parent_column_ = 0;
  std::vector<Aggregate> aggregates_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_TREE_ACCUMULATE_H_
