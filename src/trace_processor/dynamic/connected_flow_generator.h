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

#include "src/trace_processor/sqlite/db_sqlite_table.h"

#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

// Implementation of tables: CONNECTED_FLOW, FOLLOWING_FLOW, PERCEEDING_FLOW
// Searches for all entries of flow events table that are dirrectly or
// indirectly connected to the given slice (slice id). It is possible to
// restrict the direction of search.
class ConnectedFlowGenerator : public DbSqliteTable::DynamicTableGenerator {
 public:
  enum class Direction { BOTH = 0, FOLLOWING = 1, PRECEDING = 2 };

  explicit ConnectedFlowGenerator(Direction type,
                                  TraceProcessorContext* context);
  ~ConnectedFlowGenerator() override;

  Table::Schema CreateSchema() override;
  std::string TableName() override;
  uint32_t EstimateRowCount() override;
  util::Status ValidateConstraints(const QueryConstraints&) override;
  std::unique_ptr<Table> ComputeTable(const std::vector<Constraint>& cs,
                                      const std::vector<Order>& ob) override;

 private:
  // This function runs BFS on the flow events table as on directed graph
  // It starts from start_id slice and returns all flow rows that are
  // directly or indirectly connected to the starting slice.
  // If dir is FOLLOWING BFS will move in direction (slice_out -> slice_in)
  // If dir is PRECEDING BFS will move in direction (slice_in -> slice_out)
  // IMPORTANT: dir must not be set to BOTH for this method
  std::vector<uint32_t> GetConnectedFlowRows(SliceId start_id, Direction dir);

  TraceProcessorContext* context_ = nullptr;
  Direction direction_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DYNAMIC_CONNECTED_FLOW_GENERATOR_H_
