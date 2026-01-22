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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_OPERATORS_TREE_OPERATOR_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_OPERATORS_TREE_OPERATOR_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "src/trace_processor/sqlite/bindings/sqlite_module.h"

namespace perfetto::trace_processor {

class PerfettoSqlEngine;

// A value that can be stored in a tree node (mirrors SQL types).
using TreeValue = std::variant<std::monostate,  // NULL
                               int64_t,         // INTEGER
                               double,          // REAL
                               std::string>;    // TEXT/BLOB

// A tree node representing a row from the source table.
// Unlike PivotNode, this stores actual row data without aggregation.
struct TreeNode {
  // The row's ID from the id_column (used for expansion state)
  int64_t id = 0;

  // Parent's ID (-1 or 0 typically means root-level)
  std::optional<int64_t> parent_id;

  // Depth in tree: 0 for root-level nodes, 1, 2, ...
  int depth = 0;

  // All column values from the source row (in schema order)
  std::vector<TreeValue> values;

  // Tree structure
  TreeNode* parent = nullptr;
  std::vector<std::unique_ptr<TreeNode>> children;

  // Query-time state (not persisted across queries)
  bool expanded = false;
};

// Sort specification for ordering children at each level.
struct TreeSortSpec {
  // Which column to sort by (-1 for no sorting)
  int col_index = -1;

  // Sort direction
  bool descending = true;
};

// Operator table for hierarchical tree display without aggregation.
//
// Usage:
//   CREATE VIRTUAL TABLE my_tree USING __intrinsic_tree(
//       'base_table_or_subquery',
//       'id_column',         -- column containing row ID
//       'parent_id_column'   -- column containing parent ID (NULL = root)
//   );
//
// Query (whitelist mode - only specified IDs expanded):
//   SELECT * FROM my_tree
//   WHERE __expanded_ids__ = '1,2,3'   -- comma-separated node IDs to expand
//     AND __sort__ = 'name DESC'       -- sort by column name
//     AND __depth_limit__ = 3          -- max depth to show
//     AND __offset__ = 0               -- pagination offset
//     AND __limit__ = 100;             -- pagination limit
//
// Query (blacklist mode - all expanded except specified IDs):
//   SELECT * FROM my_tree
//   WHERE __collapsed_ids__ = '4,5'    -- comma-separated node IDs to collapse
//     AND __sort__ = 'size DESC';
//
// Output columns:
//   - All columns from the source table (in original order)
//   - __depth__: tree depth (0 for root-level)
//   - __tree_has_children__: 1 if node has children, 0 otherwise
//   - __child_count__: number of direct children
struct TreeOperatorModule : sqlite::Module<TreeOperatorModule> {
  // Column layout:
  // [0..num_source_cols-1] : source table columns
  // [num_source_cols]      : __depth__
  // [num_source_cols+1]    : __tree_has_children__
  // [num_source_cols+2]    : __child_count__
  // [after metadata]       : hidden columns

  // Metadata column offsets from num_source_cols
  static constexpr int kDepthOffset = 0;
  static constexpr int kTreeHasChildrenOffset = 1;
  static constexpr int kChildCountOffset = 2;
  static constexpr int kMetadataColCount = 3;

  // Hidden columns for query parameters (after metadata columns)
  enum HiddenColumn {
    kExpandedIds = 0,   // Comma-separated expanded node IDs (whitelist mode)
    kCollapsedIds = 1,  // Comma-separated collapsed node IDs (blacklist mode)
    kSortSpec = 2,      // Sort specification
    kDepthLimit = 3,    // Maximum depth to show
    kOffset = 4,        // Pagination offset
    kLimit = 5,         // Pagination limit
    kRebuild = 6,       // Trigger cache rebuild
  };
  static constexpr int kHiddenColCount = 7;

  struct Context {
    explicit Context(PerfettoSqlEngine* _engine) : engine(_engine) {}
    PerfettoSqlEngine* engine;
  };

  struct Vtab : sqlite::Module<TreeOperatorModule>::Vtab {
    PerfettoSqlEngine* engine = nullptr;

    // Configuration from CREATE TABLE
    std::string base_table;
    std::string id_column;
    std::string parent_id_column;

    // Schema information from source table
    std::vector<std::string> column_names;
    std::vector<std::string> column_types;

    // Index of id and parent_id columns in column_names
    int id_col_index = -1;
    int parent_id_col_index = -1;

    // Cached tree structure
    std::vector<std::unique_ptr<TreeNode>> roots;  // Root-level nodes
    int total_nodes = 0;

    // Flattened view of visible nodes (rebuilt on state change)
    std::vector<TreeNode*> flat;

    // Current sort specification (to avoid redundant re-sorts)
    std::string current_sort_spec;

    // Total column count for the schema
    int total_col_count = 0;
  };

  struct Cursor : sqlite::Module<TreeOperatorModule>::Cursor {
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

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_OPERATORS_TREE_OPERATOR_H_
