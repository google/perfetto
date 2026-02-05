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

// __intrinsic_rollup_tree virtual table for hierarchical grouping.
//
// This operator wraps RollupTree to expose ROLLUP-style aggregation with
// expand/collapse support as a SQLite virtual table.
//
// CREATION:
//   CREATE VIRTUAL TABLE my_rollup USING __intrinsic_rollup_tree(
//       'source_table_or_subquery',           -- Table name or (SELECT ...)
//       'col1, col2, col3',                   -- Hierarchy columns (group by)
//       'SUM(value), COUNT(*), AVG(price)'   -- Aggregation expressions
//   );
//
// QUERYING (default - all groups expanded):
//   SELECT * FROM my_rollup
//   WHERE __sort = '__agg_0 DESC'        -- Optional: sort by aggregate or
//   'name'
//     AND __offset = 0                 -- Optional: pagination offset
//     AND __limit = 100;               -- Optional: pagination limit
//
// QUERYING (allowlist mode - only specified IDs expanded):
//   SELECT * FROM my_rollup
//   WHERE __expanded_ids = '1,2,3'     -- Comma-separated node IDs to expand
//     AND __sort = '__agg_0 DESC';
//
// QUERYING (denylist mode - all expanded except specified IDs):
//   SELECT * FROM my_rollup
//   WHERE __collapsed_ids = '4,5'      -- Nodes to keep collapsed
//     AND __sort = '__agg_1 ASC';
//
// OUTPUT COLUMNS:
//   - Hierarchy columns (with NULLs like ROLLUP - deeper levels have earlier
//     columns NULL)
//   - __id: Unique node identifier
//   - __parent_id: Parent node ID (NULL for root)
//   - __depth: Tree depth (0 for root, 1 for first group level, etc.)
//   - __child_count: Number of direct children
//   - __agg_0, __agg_1, ...: Aggregated values for each aggregation expression
//
// BEHAVIOR:
//   - Root node (depth 0) contains grand totals across all data
//   - Each level groups by cumulative hierarchy columns (level 1 by col1,
//     level 2 by col1+col2, etc.)
//   - Tree is built once at CREATE time and cached
//   - By default (no expansion constraint), all groups are expanded

#include "src/trace_processor/perfetto_sql/intrinsics/operators/rollup_tree_operator.h"

#include <sqlite3.h>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/containers/rollup_tree.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {

namespace {

// Maximum number of aggregate columns supported
constexpr size_t kMaxAggCols = 32;

// Parses a comma-separated list of column names, trimming whitespace.
std::vector<std::string> ParseColumnList(const std::string& cols) {
  std::vector<std::string> result;
  for (base::StringSplitter sp(cols, ','); sp.Next();) {
    std::string col = base::TrimWhitespace(sp.cur_token());
    if (!col.empty()) {
      result.push_back(std::move(col));
    }
  }
  return result;
}

// Builds the schema declaration string for the virtual table.
std::string BuildSchemaString(const std::vector<std::string>& hierarchy_cols,
                              size_t measure_col_count) {
  std::string schema = "CREATE TABLE x(";

  // Hierarchy columns first (like ROLLUP output)
  for (size_t i = 0; i < hierarchy_cols.size(); i++) {
    if (i > 0) {
      schema += ",";
    }
    schema += hierarchy_cols[i] + " TEXT";
  }

  // Metadata columns
  schema += ",__id INTEGER";
  schema += ",__parent_id INTEGER";
  schema += ",__depth INTEGER";
  schema += ",__child_count INTEGER";

  // Add aggregate columns (no type = dynamic typing for any SQL type)
  for (size_t i = 0; i < measure_col_count; i++) {
    schema += ",__agg_" + std::to_string(i);
  }

  // Add hidden columns for query parameters
  schema += ",__aggs TEXT HIDDEN";
  schema += ",__expanded_ids TEXT HIDDEN";
  schema +=
      ",__collapsed_ids TEXT HIDDEN";  // Denylist mode (expand all except)
  schema += ",__sort TEXT HIDDEN";
  schema += ",__offset INTEGER HIDDEN";
  schema += ",__limit INTEGER HIDDEN";
  schema += ",__min_depth INTEGER HIDDEN";
  schema += ",__max_depth INTEGER HIDDEN";

  schema += ")";
  return schema;
}

// Parses a sort specification string like "__agg_0 DESC" or "__group_0 ASC".
// Format:
//   "__agg_N [ASC|DESC]" - sort all levels by aggregate N
//   "__group_N [ASC|DESC]" - sort level N by hierarchy value, others ASC
//   "" or unspecified - sort all levels alphabetically ASC (default)
RollupSortSpec ParseSortSpec(const std::string& sort_str) {
  RollupSortSpec spec;
  // Default: sort all levels alphabetically ASC (uses struct defaults)

  if (sort_str.empty()) {
    return spec;
  }

  std::string lower = base::ToLower(sort_str);
  if (lower.find("desc") != std::string::npos) {
    spec.descending = true;
  }

  // Try to extract hierarchy level from "__group_N" pattern
  size_t group_pos = lower.find("__group_");
  if (group_pos != std::string::npos) {
    size_t start = group_pos + 8;
    size_t end = start;
    while (end < lower.size() && lower[end] >= '0' && lower[end] <= '9') {
      end++;
    }
    if (end > start) {
      std::optional<int32_t> level =
          base::StringToInt32(lower.substr(start, end - start));
      if (level) {
        spec.hierarchy_level = *level;
      }
    }
    return spec;
  }

  // Try to extract aggregate index from "__agg_N" pattern
  size_t agg_pos = lower.find("__agg_");
  if (agg_pos != std::string::npos) {
    spec.hierarchy_level = -1;  // Aggregate sort, not hierarchy
    size_t start = agg_pos + 6;
    size_t end = start;
    while (end < lower.size() && lower[end] >= '0' && lower[end] <= '9') {
      end++;
    }
    if (end > start) {
      std::optional<int32_t> idx =
          base::StringToInt32(lower.substr(start, end - start));
      if (idx) {
        spec.agg_index = *idx;
      }
    }
  }

  return spec;
}

// Builds the RollupTree from the base table by executing aggregation queries.
base::Status BuildRollupTree(PerfettoSqlEngine* engine,
                             const std::string& base_table,
                             const std::vector<std::string>& hierarchy_cols,
                             const std::vector<std::string>& aggregations,
                             RollupTree* table) {
  // Build the aggregation query using UNION ALL (SQLite doesn't support ROLLUP)
  // We create one query per aggregation level and union them together.
  // Each query includes an explicit __level column to distinguish rollup levels
  // from actual NULL data values.
  std::string query;

  size_t num_hier = hierarchy_cols.size();
  size_t num_aggs = aggregations.size();

  // Grand total query (level -1): all hierarchy cols are NULL
  query += "SELECT -1 AS __level";
  for (size_t i = 0; i < num_hier; i++) {
    query += ", NULL AS " + hierarchy_cols[i];
  }
  for (size_t i = 0; i < num_aggs; i++) {
    query += ", " + aggregations[i] + " AS agg_" + std::to_string(i);
  }
  query += " FROM " + base_table;

  // One query per hierarchy level
  for (size_t level = 0; level < num_hier; level++) {
    query += " UNION ALL SELECT " + std::to_string(static_cast<int>(level)) +
             " AS __level";

    // Columns up to and including this level are real, rest are NULL
    for (size_t i = 0; i < num_hier; i++) {
      if (i <= level) {
        query += ", " + hierarchy_cols[i];
      } else {
        query += ", NULL AS " + hierarchy_cols[i];
      }
    }

    // Aggregates - use expressions directly
    for (size_t i = 0; i < num_aggs; i++) {
      query += ", " + aggregations[i] + " AS agg_" + std::to_string(i);
    }

    query += " FROM " + base_table + " GROUP BY ";
    for (size_t i = 0; i <= level; i++) {
      if (i > 0) {
        query += ", ";
      }
      query += hierarchy_cols[i];
    }
  }

  // Execute the query
  auto result = engine->ExecuteUntilLastStatement(
      SqlSource::FromTraceProcessorImplementation(query));
  if (!result.ok()) {
    return result.status();
  }

  auto& stmt = result->stmt;

  // Helper lambda to process a single row from the statement.
  // Column layout: [__level, hier_0, hier_1, ..., agg_0, agg_1, ...]
  auto process_row = [&]() {
    // Read explicit level from column 0 (supports NULL as valid data)
    int level = static_cast<int>(
        sqlite3_column_int64(stmt.sqlite_stmt(), 0));

    // Read hierarchy values up to and including level (type-aware)
    // Hierarchy columns start at index 1 (after __level)
    std::vector<RollupValue> segments;
    for (int i = 0; i <= level; i++) {
      int col_idx = 1 + i;  // Skip __level column
      int sql_type = sqlite3_column_type(stmt.sqlite_stmt(), col_idx);
      RollupValue val;

      switch (sql_type) {
        case SQLITE_INTEGER:
          val = sqlite3_column_int64(stmt.sqlite_stmt(), col_idx);
          break;
        case SQLITE_FLOAT:
          val = sqlite3_column_double(stmt.sqlite_stmt(), col_idx);
          break;
        case SQLITE_TEXT: {
          const char* text = reinterpret_cast<const char*>(
              sqlite3_column_text(stmt.sqlite_stmt(), col_idx));
          val = std::string(text ? text : "");
          break;
        }
        case SQLITE_NULL:
        default:
          val = std::monostate{};
          break;
      }
      segments.push_back(std::move(val));
    }

    // Get aggregate values (type-aware)
    // Aggregate columns start at index 1 + num_hier (after __level and hierarchy)
    std::vector<RollupValue> aggs;
    for (size_t i = 0; i < num_aggs; i++) {
      int col_idx = static_cast<int>(1 + num_hier + i);
      int sql_type = sqlite3_column_type(stmt.sqlite_stmt(), col_idx);
      RollupValue val;

      switch (sql_type) {
        case SQLITE_INTEGER:
          val = sqlite3_column_int64(stmt.sqlite_stmt(), col_idx);
          break;
        case SQLITE_FLOAT:
          val = sqlite3_column_double(stmt.sqlite_stmt(), col_idx);
          break;
        case SQLITE_TEXT: {
          const char* text = reinterpret_cast<const char*>(
              sqlite3_column_text(stmt.sqlite_stmt(), col_idx));
          val = std::string(text ? text : "");
          break;
        }
        case SQLITE_NULL:
        default:
          val = std::monostate{};
          break;
      }
      aggs.push_back(std::move(val));
    }

    if (level < 0) {
      // This is the grand total row - store in root
      table->SetRootAggregates(std::move(aggs));
    } else {
      // Add row to the tree
      table->AddRow(level, segments, std::move(aggs));
    }
  };

  // ExecuteUntilLastStatement already stepped once, so if stmt is not done,
  // the first row is ready to be read. Process it before calling Step() again.
  if (!stmt.IsDone()) {
    process_row();
  }

  // Process remaining rows
  while (stmt.Step()) {
    process_row();
  }

  if (!stmt.status().ok()) {
    return stmt.status();
  }

  return base::OkStatus();
}

}  // namespace

int RollupTreeOperatorModule::Create(sqlite3* db,
                                     void* raw_ctx,
                                     int argc,
                                     const char* const* argv,
                                     sqlite3_vtab** vtab,
                                     char** pzErr) {
  // argv[0] = module name
  // argv[1] = database name
  // argv[2] = table name
  // argv[3] = base table
  // argv[4] = hierarchy columns
  // argv[5] = aggregation expressions (e.g., "SUM(col1), COUNT(*), AVG(col2)")

  if (argc < 6) {
    *pzErr = sqlite3_mprintf(
        "__intrinsic_rollup_tree requires 3 arguments: base_table, "
        "hierarchy_cols, aggregations");
    return SQLITE_ERROR;
  }

  auto* ctx = GetContext(raw_ctx);

  std::string base_table = argv[3];
  // Remove surrounding quotes if present
  if (base_table.size() >= 2 &&
      ((base_table.front() == '\'' && base_table.back() == '\'') ||
       (base_table.front() == '"' && base_table.back() == '"'))) {
    base_table = base_table.substr(1, base_table.size() - 2);
  }

  std::string hierarchy_str = argv[4];
  if (hierarchy_str.size() >= 2 &&
      ((hierarchy_str.front() == '\'' && hierarchy_str.back() == '\'') ||
       (hierarchy_str.front() == '"' && hierarchy_str.back() == '"'))) {
    hierarchy_str = hierarchy_str.substr(1, hierarchy_str.size() - 2);
  }

  std::string agg_str = argv[5];
  if (agg_str.size() >= 2 &&
      ((agg_str.front() == '\'' && agg_str.back() == '\'') ||
       (agg_str.front() == '"' && agg_str.back() == '"'))) {
    agg_str = agg_str.substr(1, agg_str.size() - 2);
  }

  std::vector<std::string> hierarchy_cols = ParseColumnList(hierarchy_str);
  std::vector<std::string> aggregations = ParseColumnList(agg_str);

  if (hierarchy_cols.empty()) {
    *pzErr = sqlite3_mprintf("At least one hierarchy column is required");
    return SQLITE_ERROR;
  }

  if (aggregations.empty()) {
    *pzErr = sqlite3_mprintf("At least one aggregation is required");
    return SQLITE_ERROR;
  }

  if (aggregations.size() > kMaxAggCols) {
    *pzErr = sqlite3_mprintf("Maximum %zu aggregations supported", kMaxAggCols);
    return SQLITE_ERROR;
  }

  // Build and declare schema
  std::string schema = BuildSchemaString(hierarchy_cols, aggregations.size());
  if (int ret = sqlite3_declare_vtab(db, schema.c_str()); ret != SQLITE_OK) {
    return ret;
  }

  // Create the vtab
  auto res = std::make_unique<Vtab>();
  res->engine = ctx->engine;
  res->base_table = std::move(base_table);
  res->aggregations = std::move(aggregations);

  // Column layout: hierarchy cols + 4 metadata + agg cols + 8 hidden
  size_t num_hier = hierarchy_cols.size();
  res->total_col_count = static_cast<int>(num_hier + kMetadataColCount +
                                          res->aggregations.size() + 8);

  // Create the RollupTree
  res->table = std::make_unique<RollupTree>(std::move(hierarchy_cols),
                                            res->aggregations.size());

  // Build the tree from base table
  base::Status status = BuildRollupTree(ctx->engine, res->base_table,
                                        res->table->hierarchy_cols(),
                                        res->aggregations, res->table.get());
  if (!status.ok()) {
    *pzErr = sqlite3_mprintf("%s", status.c_message());
    return SQLITE_ERROR;
  }

  *vtab = res.release();
  return SQLITE_OK;
}

int RollupTreeOperatorModule::Destroy(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> tab(GetVtab(vtab));
  return SQLITE_OK;
}

int RollupTreeOperatorModule::Connect(sqlite3* db,
                                      void* raw_ctx,
                                      int argc,
                                      const char* const* argv,
                                      sqlite3_vtab** vtab,
                                      char** pzErr) {
  return Create(db, raw_ctx, argc, argv, vtab, pzErr);
}

int RollupTreeOperatorModule::Disconnect(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> tab(GetVtab(vtab));
  return SQLITE_OK;
}

int RollupTreeOperatorModule::BestIndex(sqlite3_vtab* vtab,
                                        sqlite3_index_info* info) {
  auto* t = GetVtab(vtab);

  // Calculate the column indices for hidden columns
  // Layout: hierarchy cols + metadata cols + aggregate cols + hidden cols
  int num_hier = static_cast<int>(t->table->hierarchy_cols().size());
  int hidden_start = num_hier + kMetadataColCount +
                     static_cast<int>(t->table->num_aggregates());
  int aggs_col = hidden_start + kAggsSpec;
  int expanded_col = hidden_start + kExpandedIds;
  int collapsed_col = hidden_start + kCollapsedIds;
  int sort_col = hidden_start + kSortSpec;
  int offset_col = hidden_start + kOffset;
  int limit_col = hidden_start + kLimit;
  int min_depth_col = hidden_start + kMinDepth;
  int max_depth_col = hidden_start + kMaxDepth;

  // Build idxStr to encode argv index for each constraint type.
  // Format: 8 characters, one per constraint type (aggs, expanded, collapsed,
  // sort, offset, limit, min_depth, max_depth). Each char is '0'-'7' indicating
  // the argv index, or '-' if not present.
  // This allows Filter() to know exactly which argv slot each value is in.
  char idx_flags[9] = "--------";

  int argv_index = 1;  // argvIndex is 1-based in SQLite
  for (int i = 0; i < info->nConstraint; i++) {
    if (!info->aConstraint[i].usable) {
      continue;
    }
    if (!sqlite::utils::IsOpEq(info->aConstraint[i].op)) {
      continue;
    }

    int col = info->aConstraint[i].iColumn;
    if (col == aggs_col) {
      idx_flags[0] = static_cast<char>('0' + argv_index - 1);  // Store 0-based
      info->aConstraintUsage[i].argvIndex = argv_index++;
      info->aConstraintUsage[i].omit = true;
    } else if (col == expanded_col) {
      idx_flags[1] = static_cast<char>('0' + argv_index - 1);
      info->aConstraintUsage[i].argvIndex = argv_index++;
      info->aConstraintUsage[i].omit = true;
    } else if (col == collapsed_col) {
      idx_flags[2] = static_cast<char>('0' + argv_index - 1);
      info->aConstraintUsage[i].argvIndex = argv_index++;
      info->aConstraintUsage[i].omit = true;
    } else if (col == sort_col) {
      idx_flags[3] = static_cast<char>('0' + argv_index - 1);
      info->aConstraintUsage[i].argvIndex = argv_index++;
      info->aConstraintUsage[i].omit = true;
    } else if (col == offset_col) {
      idx_flags[4] = static_cast<char>('0' + argv_index - 1);
      info->aConstraintUsage[i].argvIndex = argv_index++;
      info->aConstraintUsage[i].omit = true;
    } else if (col == limit_col) {
      idx_flags[5] = static_cast<char>('0' + argv_index - 1);
      info->aConstraintUsage[i].argvIndex = argv_index++;
      info->aConstraintUsage[i].omit = true;
    } else if (col == min_depth_col) {
      idx_flags[6] = static_cast<char>('0' + argv_index - 1);
      info->aConstraintUsage[i].argvIndex = argv_index++;
      info->aConstraintUsage[i].omit = true;
    } else if (col == max_depth_col) {
      idx_flags[7] = static_cast<char>('0' + argv_index - 1);
      info->aConstraintUsage[i].argvIndex = argv_index++;
      info->aConstraintUsage[i].omit = true;
    }
  }

  info->idxStr = sqlite3_mprintf("%s", idx_flags);
  info->needToFreeIdxStr = true;
  info->estimatedCost = 1000.0;

  return SQLITE_OK;
}

int RollupTreeOperatorModule::Open(sqlite3_vtab*,
                                   sqlite3_vtab_cursor** cursor) {
  auto c = std::make_unique<Cursor>();
  *cursor = c.release();
  return SQLITE_OK;
}

int RollupTreeOperatorModule::Close(sqlite3_vtab_cursor* cursor) {
  delete GetCursor(cursor);
  return SQLITE_OK;
}

int RollupTreeOperatorModule::Filter(sqlite3_vtab_cursor* cursor,
                                     int /*idxNum*/,
                                     const char* idxStr,
                                     int argc,
                                     sqlite3_value** argv) {
  auto* t = GetVtab(cursor->pVtab);
  auto* c = GetCursor(cursor);

  // Reset cursor state
  c->row_index = 0;
  c->offset = 0;
  c->limit = std::numeric_limits<int>::max();
  c->rows_returned = 0;

  // Build flatten options
  RollupFlattenOptions options;
  options.denylist_mode = false;
  bool expansion_specified = false;

  // Parse idxStr to determine which arguments are present and their argv index.
  // Each char in idxStr is either '-' (not present) or '0'-'7' (argv index).
  std::string flags = idxStr ? idxStr : "--------";

  std::string sort_spec_str;

  // Helper to get argv value for a flag position, or nullptr if not present
  auto get_argv = [&](size_t flag_pos) -> sqlite3_value* {
    if (flag_pos >= flags.size() || flags[flag_pos] == '-') {
      return nullptr;
    }
    int argv_idx = flags[flag_pos] - '0';
    if (argv_idx < 0 || argv_idx >= argc) {
      return nullptr;
    }
    return argv[argv_idx];
  };

  // Helper to parse comma-separated IDs
  auto parse_ids = [&](const char* ids_str) {
    if (ids_str) {
      for (base::StringSplitter sp(ids_str, ','); sp.Next();) {
        std::string id_str = base::TrimWhitespace(sp.cur_token());
        if (!id_str.empty()) {
          std::optional<int64_t> id = base::StringToInt64(id_str);
          if (id) {
            options.ids.insert(*id);
          }
        }
      }
    }
  };

  // Process __aggs (flag position 0)
  // Currently unused, but could parse to select specific aggregates

  // Process __expanded_ids (flag position 1) - allowlist mode
  if (sqlite3_value* val = get_argv(1)) {
    const char* ids_str =
        reinterpret_cast<const char*>(sqlite3_value_text(val));
    parse_ids(ids_str);
    options.denylist_mode = false;
    expansion_specified = true;
  }

  // Process __collapsed_ids (flag position 2) - denylist mode (expand all
  // except). Note: If both expanded_ids and collapsed_ids are provided,
  // collapsed_ids wins
  if (sqlite3_value* val = get_argv(2)) {
    const char* ids_str =
        reinterpret_cast<const char*>(sqlite3_value_text(val));
    options.ids.clear();
    parse_ids(ids_str);
    options.denylist_mode = true;
    expansion_specified = true;
  }

  // Default: expand all groups when no expansion constraint is specified
  if (!expansion_specified) {
    options.denylist_mode = true;  // Denylist with empty set = expand all
  }

  // Process __sort (flag position 3)
  if (sqlite3_value* val = get_argv(3)) {
    const char* sort_str =
        reinterpret_cast<const char*>(sqlite3_value_text(val));
    if (sort_str) {
      sort_spec_str = sort_str;
    }
  }

  // Process __offset (flag position 4)
  if (sqlite3_value* val = get_argv(4)) {
    options.offset = sqlite3_value_int(val);
    c->offset = options.offset;
  }

  // Process __limit (flag position 5)
  if (sqlite3_value* val = get_argv(5)) {
    options.limit = sqlite3_value_int(val);
    c->limit = options.limit;
  }

  // Process __min_depth (flag position 6)
  if (sqlite3_value* val = get_argv(6)) {
    options.min_depth = sqlite3_value_int(val);
  }

  // Process __max_depth (flag position 7)
  if (sqlite3_value* val = get_argv(7)) {
    options.max_depth = sqlite3_value_int(val);
  }

  // Parse sort spec (default to "__agg_0 DESC")
  if (sort_spec_str.empty()) {
    sort_spec_str = "__agg_0 DESC";
  }
  options.sort = ParseSortSpec(sort_spec_str);

  // Get flattened rows from RollupTree
  t->flat_rows = t->table->GetRows(options);

  // Row index starts at 0 (pagination already applied by GetRows)
  c->row_index = 0;

  return SQLITE_OK;
}

int RollupTreeOperatorModule::Next(sqlite3_vtab_cursor* cursor) {
  auto* c = GetCursor(cursor);
  c->row_index++;
  c->rows_returned++;
  return SQLITE_OK;
}

int RollupTreeOperatorModule::Eof(sqlite3_vtab_cursor* cursor) {
  auto* t = GetVtab(cursor->pVtab);
  auto* c = GetCursor(cursor);

  if (c->row_index >= static_cast<int>(t->flat_rows.size())) {
    return 1;
  }
  return 0;
}

int RollupTreeOperatorModule::Column(sqlite3_vtab_cursor* cursor,
                                     sqlite3_context* ctx,
                                     int col) {
  auto* t = GetVtab(cursor->pVtab);
  auto* c = GetCursor(cursor);

  if (c->row_index >= static_cast<int>(t->flat_rows.size())) {
    sqlite::result::Null(ctx);
    return SQLITE_OK;
  }

  const RollupFlatRow& row = t->flat_rows[static_cast<size_t>(c->row_index)];
  int num_hier = static_cast<int>(t->table->hierarchy_cols().size());

  // Column layout:
  // [0..num_hier-1]: hierarchy columns (with NULLs like ROLLUP)
  // [num_hier+0]: __id
  // [num_hier+1]: __parent_id
  // [num_hier+2]: __depth
  // [num_hier+3]: __child_count
  // [num_hier+4..]: __agg_0, __agg_1, ...

  if (col < num_hier) {
    // Hierarchy column - return value if level >= col, else NULL
    size_t hier_idx = static_cast<size_t>(col);
    if (row.depth >= col && hier_idx < row.hierarchy_values.size()) {
      const RollupValue& val = row.hierarchy_values[hier_idx];
      if (std::holds_alternative<std::monostate>(val)) {
        sqlite::result::Null(ctx);
      } else if (std::holds_alternative<int64_t>(val)) {
        sqlite::result::Long(ctx, std::get<int64_t>(val));
      } else if (std::holds_alternative<double>(val)) {
        sqlite::result::Double(ctx, std::get<double>(val));
      } else if (std::holds_alternative<std::string>(val)) {
        const std::string& str = std::get<std::string>(val);
        sqlite::result::StaticString(ctx, str.c_str(),
                                     static_cast<int>(str.size()));
      }
    } else {
      sqlite::result::Null(ctx);
    }
  } else if (col == num_hier + kIdOffset) {
    sqlite::result::Long(ctx, row.id);
  } else if (col == num_hier + kParentIdOffset) {
    if (row.parent_id >= 0) {
      sqlite::result::Long(ctx, row.parent_id);
    } else {
      sqlite::result::Null(ctx);
    }
  } else if (col == num_hier + kDepthOffset) {
    sqlite::result::Long(ctx, row.depth);
  } else if (col == num_hier + kChildCountOffset) {
    sqlite::result::Long(ctx, static_cast<int64_t>(row.child_count));
  } else {
    // Aggregate or hidden column
    int agg_start = num_hier + kMetadataColCount;
    int agg_end = agg_start + static_cast<int>(t->table->num_aggregates());
    if (col >= agg_start && col < agg_end) {
      size_t agg_idx = static_cast<size_t>(col - agg_start);
      if (agg_idx < row.aggregates.size()) {
        const RollupValue& val = row.aggregates[agg_idx];
        if (std::holds_alternative<std::monostate>(val)) {
          sqlite::result::Null(ctx);
        } else if (std::holds_alternative<int64_t>(val)) {
          sqlite::result::Long(ctx, std::get<int64_t>(val));
        } else if (std::holds_alternative<double>(val)) {
          sqlite::result::Double(ctx, std::get<double>(val));
        } else if (std::holds_alternative<std::string>(val)) {
          const std::string& str = std::get<std::string>(val);
          sqlite::result::StaticString(ctx, str.c_str(),
                                       static_cast<int>(str.size()));
        }
      } else {
        sqlite::result::Null(ctx);
      }
    } else {
      // Hidden columns - return NULL
      sqlite::result::Null(ctx);
    }
  }

  return SQLITE_OK;
}

int RollupTreeOperatorModule::Rowid(sqlite3_vtab_cursor* cursor,
                                    sqlite_int64* rowid) {
  auto* c = GetCursor(cursor);
  *rowid = c->row_index;
  return SQLITE_OK;
}

}  // namespace perfetto::trace_processor
