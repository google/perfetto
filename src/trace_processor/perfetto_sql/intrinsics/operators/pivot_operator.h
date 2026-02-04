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
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "src/trace_processor/containers/pivot_table.h"
#include "src/trace_processor/sqlite/bindings/sqlite_module.h"

namespace perfetto::trace_processor {

class PerfettoSqlEngine;

// SQLite virtual table module for hierarchical pivot functionality.
//
// This module wraps PivotTable to expose it as a SQLite virtual table,
// allowing SQL queries with expand/collapse, sorting, and pagination.
//
// Usage:
//   CREATE VIRTUAL TABLE my_pivot USING __intrinsic_pivot(
//       'base_table',
//       'col1, col2, col3',                    -- hierarchy columns
//       'SUM(value), COUNT(*), AVG(price)'    -- aggregation expressions
//   );
//
// Query (default - all groups expanded):
//   SELECT * FROM my_pivot
//   WHERE __sort = '__agg_0 DESC'        -- sort by aggregate or 'name'
//     AND __offset = 0                   -- pagination offset
//     AND __limit = 100;                 -- pagination limit
//
// Query (allowlist mode - only specified IDs expanded):
//   SELECT * FROM my_pivot
//   WHERE __expanded_ids = '1,2,3'       -- comma-separated node IDs to expand
//     AND __sort = '__agg_0 DESC';
//
// Query (denylist mode - all expanded except specified IDs):
//   SELECT * FROM my_pivot
//   WHERE __collapsed_ids = '4,5'        -- comma-separated node IDs to
//   collapse
//     AND __sort = '__agg_0 DESC';
struct PivotOperatorModule : sqlite::Module<PivotOperatorModule> {
  // Column layout (indices computed at runtime based on hierarchy_cols.size()):
  // [0..num_hier-1]         : hierarchy columns (with NULLs like ROLLUP)
  // [num_hier]              : __id
  // [num_hier+1]            : __parent_id
  // [num_hier+2]            : __depth
  // [num_hier+3]            : __child_count
  // [num_hier+4..+4+num_agg]: __agg_0, __agg_1, ...
  // [after aggs]            : hidden columns

  // Metadata column offsets from num_hier
  static constexpr int kIdOffset = 0;
  static constexpr int kParentIdOffset = 1;
  static constexpr int kDepthOffset = 2;
  static constexpr int kChildCountOffset = 3;
  static constexpr int kMetadataColCount = 4;

  // Hidden columns for query parameters (after aggregate columns)
  enum HiddenColumn {
    kAggsSpec = 0,      // Aggregate specification (e.g., "SUM(col1), COUNT(*)")
    kExpandedIds = 1,   // Comma-separated expanded node IDs (allowlist mode)
    kCollapsedIds = 2,  // Comma-separated collapsed node IDs (denylist mode)
    kSortSpec = 3,      // Sort specification
    kOffset = 4,        // Pagination offset
    kLimit = 5,         // Pagination limit
  };

  struct Context {
    explicit Context(PerfettoSqlEngine* _engine) : engine(_engine) {}
    PerfettoSqlEngine* engine;
  };

  struct Vtab : sqlite::Module<PivotOperatorModule>::Vtab {
    PerfettoSqlEngine* engine = nullptr;

    // Configuration from CREATE TABLE
    std::string base_table;
    std::vector<std::string> aggregations;  // e.g., "SUM(col)", "COUNT(*)"

    // The pivot table with all tree logic
    std::unique_ptr<PivotTable> table;

    // Cached flattened rows for current query
    std::vector<PivotFlatRow> flat_rows;

    // Total column count for the schema
    int total_col_count = 0;
  };

  struct Cursor : sqlite::Module<PivotOperatorModule>::Cursor {
    // Current position in flat_rows array
    int row_index = 0;

    // Pagination parameters
    int offset = 0;
    int limit = std::numeric_limits<int>::max();
    int rows_returned = 0;
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
