/*
 * Copyright (C) 2020 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DYNAMIC_CONNECTED_FLOW_GENERATOR_H_
#define SRC_TRACE_PROCESSOR_DYNAMIC_CONNECTED_FLOW_GENERATOR_H_

#include "src/trace_processor/dynamic/dynamic_table_generator.h"
#include "src/trace_processor/storage/trace_storage.h"

#include <queue>
#include <set>

namespace perfetto {
namespace trace_processor {
namespace tables {

#define PERFETTO_TP_CONNECTED_FLOW_TABLE_DEF(NAME, PARENT, C) \
  NAME(ConnectedFlowTable, "not_exposed_to_sql")              \
  PARENT(PERFETTO_TP_FLOW_TABLE_DEF, C)                       \
  C(uint32_t, start_id, Column::Flag::kHidden)

PERFETTO_TP_TABLE(PERFETTO_TP_CONNECTED_FLOW_TABLE_DEF);

}  // namespace tables

class TraceProcessorContext;

// Implementation of tables:
// - DIRECTLY_CONNECTED_FLOW
// - PRECEDING_FLOW
// - FOLLOWING_FLOW
class ConnectedFlowGenerator : public DynamicTableGenerator {
 public:
  enum class Mode {
    // Directly connected slices through the same flow ID given by the trace
    // writer.
    kDirectlyConnectedFlow,
    // Flow events which can be reached from the given slice by going over
    // incoming flow events or to parent slices.
    kPrecedingFlow,
    // Flow events which can be reached from the given slice by going over
    // outgoing flow events or to child slices.
    kFollowingFlow,
  };

  ConnectedFlowGenerator(Mode mode, const TraceStorage*);
  ~ConnectedFlowGenerator() override;

  Table::Schema CreateSchema() override;
  std::string TableName() override;
  uint32_t EstimateRowCount() override;
  base::Status ValidateConstraints(const QueryConstraints&) override;
  base::Status ComputeTable(const std::vector<Constraint>& cs,
                            const std::vector<Order>& ob,
                            const BitVector& cols_used,
                            std::unique_ptr<Table>& table_return) override;

 private:
  Mode mode_;
  const TraceStorage* storage_ = nullptr;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DYNAMIC_CONNECTED_FLOW_GENERATOR_H_
