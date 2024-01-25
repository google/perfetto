/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TABLE_FUNCTIONS_DOMINATOR_TREE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TABLE_FUNCTIONS_DOMINATOR_TREE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/static_table_function.h"

namespace perfetto::trace_processor {

// An SQL table-function which computes the dominator-tree [1] of a graph.
//
// Arguments:
//  1) |source_node_ids|: RepeatedBuilderResult proto containing a column of
//     int64 values corresponding to the source of edges.
//  2) |dest_node_ids|:  RepeatedBuilderResult proto containing a column of
//     int64 values corresponding to the destination of edges. This number of
//     values should be the same as |source_node_ids| with each index in
//     |source_node_ids| acting as the source for the corresponding index in
//     |dest_node_ids|.
//  2) |start_node_id|:  ID of the "start" node in the graph which should be the
//     root of the dominator tree.
//
// Returns:
//  A table with the dominator tree of the input graph. The schema of the table
//  is (node_id int64_t, dominator_node_id optional<int64_t>).
//
// Note: as this function takes table columns as an argument, it is not intended
// to be used directly from SQL: instead a "dominator_tree" macro exists in
// the standard library, wrapping it and making it user-friendly.
//
// Implementation notes:
// This class implements the Lengauer-Tarjan Dominators algorithm [2]. This was
// chosen as it runs on O(nlog(n)) time: as we expect this class to be used on
// large tables (i.e. tables containing Java heap graphs), it's important that
// the code is efficient.
//
// As Lengauer-Tarjan Dominators is not the most intuitive algorithm [3] might
// be a useful resource for grasping the key principles behind it.
//
// [1] https://en.wikipedia.org/wiki/Dominator_(graph_theory)
// [2] https://dl.acm.org/doi/10.1145/357062.357071
// [3] TODO(lalitm): link to the blog post once it's been written.
class DominatorTree : public StaticTableFunction {
 public:
  explicit DominatorTree(StringPool*);
  virtual ~DominatorTree() override;

  // StaticTableFunction implementation.
  Table::Schema CreateSchema() override;
  std::string TableName() override;
  uint32_t EstimateRowCount() override;
  base::StatusOr<std::unique_ptr<Table>> ComputeTable(
      const std::vector<SqlValue>& arguments) override;

 private:
  StringPool* pool_ = nullptr;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TABLE_FUNCTIONS_DOMINATOR_TREE_H_
