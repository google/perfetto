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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_OPERATORS_PIVOT_OPERATOR_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_OPERATORS_PIVOT_OPERATOR_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "src/trace_processor/sqlite/bindings/sqlite_module.h"

namespace perfetto::trace_processor {

class PerfettoSqlEngine;

// A hierarchical pivot node representing a group in the pivot table.
struct PivotNode {
  // Unique node ID (assigned during tree build)
  int64_t id = 0;

  // Depth in tree: 0, 1, 2, ...
  int level = 0;

  // Hierarchy column values at each level (for ROLLUP-style output)
  // Values up to and including 'level' are set, rest are empty (NULL)
  std::vector<std::string> hierarchy_values;

  // Aggregate values, one per measure column
  std::vector<double> aggs;

  // Tree structure
  PivotNode* parent = nullptr;
  std::vector<std::unique_ptr<PivotNode>> children;

  // Query-time state (not persisted across queries)
  bool expanded = false;
};

// Sort specification for ordering children at each level.
struct PivotSortSpec {
  // Which aggregate to sort by (-1 for sorting by name)
  int agg_index = -1;

  // Sort direction
  bool descending = true;
};

// Operator table for hierarchical pivot functionality.
//
// Usage:
//   CREATE VIRTUAL TABLE my_pivot USING __intrinsic_pivot(
//       'base_table',
//       'col1, col2, col3',    -- hierarchy columns
//       'measure1, measure2'   -- measure columns
//   );
//
// Query:
//   SELECT * FROM my_pivot
//   WHERE __expanded_ids__ = '1,2,3'   -- comma-separated node IDs to expand
//     AND __sort__ = 'agg_0 DESC'      -- sort by aggregate or 'name'
//     AND __depth_limit__ = 3          -- max depth to show
//     AND __offset__ = 0               -- pagination offset
//     AND __limit__ = 100;             -- pagination limit
struct PivotOperatorModule : sqlite::Module<PivotOperatorModule> {
  // Column layout (indices computed at runtime based on hierarchy_cols.size()):
  // [0..num_hier-1]         : hierarchy columns (with NULLs like ROLLUP)
  // [num_hier]              : __id__
  // [num_hier+1]            : __parent_id__
  // [num_hier+2]            : __depth__
  // [num_hier+3]            : __has_children__
  // [num_hier+4]            : __child_count__
  // [num_hier+5..+5+num_agg]: agg_0, agg_1, ...
  // [after aggs]            : hidden columns

  // Metadata column offsets from num_hier
  static constexpr int kIdOffset = 0;
  static constexpr int kParentIdOffset = 1;
  static constexpr int kDepthOffset = 2;
  static constexpr int kHasChildrenOffset = 3;
  static constexpr int kChildCountOffset = 4;
  static constexpr int kMetadataColCount = 5;

  // Hidden columns for query parameters (after aggregate columns)
  enum HiddenColumn {
    kAggsSpec = 0,       // Aggregate specification (e.g., "SUM(col1), COUNT(*)")
    kExpandedIds = 1,    // Comma-separated expanded node IDs
    kSortSpec = 2,       // Sort specification
    kDepthLimit = 3,     // Maximum depth to show
    kOffset = 4,         // Pagination offset
    kLimit = 5,          // Pagination limit
    kRebuild = 6,        // Trigger cache rebuild
  };

  struct Context {
    explicit Context(PerfettoSqlEngine* _engine) : engine(_engine) {}
    PerfettoSqlEngine* engine;
  };

  struct Vtab : sqlite::Module<PivotOperatorModule>::Vtab {
    PerfettoSqlEngine* engine = nullptr;

    // Configuration from CREATE TABLE
    std::string base_table;
    std::vector<std::string> hierarchy_cols;
    std::vector<std::string> measure_cols;

    // Cached tree structure
    std::unique_ptr<PivotNode> root;
    int total_nodes = 0;

    // Flattened view of visible nodes (rebuilt on state change)
    std::vector<PivotNode*> flat;
    bool flat_dirty = true;

    // Number of aggregate columns in output
    size_t agg_col_count = 0;

    // Total column count for the schema
    int total_col_count = 0;
  };

  struct Cursor : sqlite::Module<PivotOperatorModule>::Cursor {
    // Current position in flat array
    int row_index = 0;

    // Pagination parameters
    int offset = 0;
    int limit = std::numeric_limits<int>::max();
    int rows_returned = 0;

    // Depth limit for query
    int depth_limit = std::numeric_limits<int>::max();
  };

  static constexpr auto kType = kCreateOnly;
  static constexpr bool kSupportsWrites = false;
  static constexpr bool kDoesOverloadFunctions = false;

  static int Create(sqlite3*,
                    void*,
                    int,
                    const char* const*,
                    sqlite3_vtab**,
                    char**);
  static int Destroy(sqlite3_vtab*);

  static int Connect(sqlite3*,
                     void*,
                     int,
                     const char* const*,
                     sqlite3_vtab**,
                     char**);
  static int Disconnect(sqlite3_vtab*);

  static int BestIndex(sqlite3_vtab*, sqlite3_index_info*);

  static int Open(sqlite3_vtab*, sqlite3_vtab_cursor**);
  static int Close(sqlite3_vtab_cursor*);

  static int Filter(sqlite3_vtab_cursor*,
                    int,
                    const char*,
                    int,
                    sqlite3_value**);
  static int Next(sqlite3_vtab_cursor*);
  static int Eof(sqlite3_vtab_cursor*);
  static int Column(sqlite3_vtab_cursor*, sqlite3_context*, int);
  static int Rowid(sqlite3_vtab_cursor*, sqlite_int64*);

  // This needs to happen at the end as it depends on the functions
  // defined above.
  static constexpr sqlite3_module kModule = CreateModule();
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_OPERATORS_PIVOT_OPERATOR_H_
