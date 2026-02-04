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

// __intrinsic_pivot virtual table for hierarchical pivot/grouping.
//
// This operator performs ROLLUP-style aggregation with expand/collapse support,
// building a tree where each level groups by a different hierarchy column.
//
// CREATION:
//   CREATE VIRTUAL TABLE my_pivot USING __intrinsic_pivot(
//       'source_table_or_subquery',           -- Table name or (SELECT ...)
//       'col1, col2, col3',                   -- Hierarchy columns (group by)
//       'SUM(value), COUNT(*), AVG(price)'   -- Aggregation expressions
//   );
//
// QUERYING (default - all groups expanded):
//   SELECT * FROM my_pivot
//   WHERE __sort = 'agg_0 DESC'        -- Optional: sort by aggregate or 'name'
//     AND __offset = 0                 -- Optional: pagination offset
//     AND __limit = 100;               -- Optional: pagination limit
//
// QUERYING (allowlist mode - only specified IDs expanded):
//   SELECT * FROM my_pivot
//   WHERE __expanded_ids = '1,2,3'     -- Comma-separated node IDs to expand
//     AND __sort = 'agg_0 DESC';
//
// QUERYING (denylist mode - all expanded except specified IDs):
//   SELECT * FROM my_pivot
//   WHERE __collapsed_ids = '4,5'      -- Nodes to keep collapsed
//     AND __sort = 'agg_1 ASC';
//
// OUTPUT COLUMNS:
//   - Hierarchy columns (with NULLs like ROLLUP - deeper levels have earlier
//     columns NULL)
//   - __id: Unique node identifier
//   - __parent_id: Parent node ID (NULL for root)
//   - __depth: Tree depth (0 for root, 1 for first group level, etc.)
//   - __has_children: 1 if node has children, 0 otherwise
//   - __child_count: Number of direct children
//   - agg_0, agg_1, ...: Aggregated values for each aggregation expression
//
// BEHAVIOR:
//   - Root node (depth 0) contains grand totals across all data
//   - Each level groups by cumulative hierarchy columns (level 1 by col1,
//     level 2 by col1+col2, etc.)
//   - Tree is built once at CREATE time and cached
//   - By default (no expansion constraint), all groups are expanded

#include "src/trace_processor/perfetto_sql/intrinsics/operators/pivot_operator.h"

#include <sqlite3.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {

namespace {

// Maximum number of aggregate columns supported
constexpr size_t kMaxAggCols = 32;

// Convert a PivotValue to a sortable double for comparison.
double PivotValueToDouble(const PivotValue& val) {
  if (std::holds_alternative<std::monostate>(val)) {
    return std::numeric_limits<double>::lowest();
  }
  if (std::holds_alternative<int64_t>(val)) {
    return static_cast<double>(std::get<int64_t>(val));
  }
  if (std::holds_alternative<double>(val)) {
    return std::get<double>(val);
  }
  // For strings, return 0 (can't meaningfully convert to double)
  return 0.0;
}

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
  schema += ",__has_children INTEGER";
  schema += ",__child_count INTEGER";

  // Add aggregate columns (no type = dynamic typing for any SQL type)
  for (size_t i = 0; i < measure_col_count; i++) {
    schema += ",agg_" + std::to_string(i);
  }

  // Add hidden columns for query parameters
  schema += ",__aggs TEXT HIDDEN";
  schema += ",__expanded_ids TEXT HIDDEN";
  schema += ",__collapsed_ids TEXT HIDDEN";  // Denylist mode (expand all except)
  schema += ",__sort TEXT HIDDEN";
  schema += ",__offset INTEGER HIDDEN";
  schema += ",__limit INTEGER HIDDEN";

  schema += ")";
  return schema;
}

// Finds or creates a node at the given path in the tree.
// num_hier is the total number of hierarchy columns for setting up
// hierarchy_values. next_id is incremented for each new node created.
PivotNode* FindOrCreateNode(PivotNode* root,
                            const std::vector<std::string>& segments,
                            int level,
                            size_t num_hier,
                            int64_t* next_id) {
  if (segments.empty() || level < 0) {
    return root;
  }

  PivotNode* current = root;
  for (int i = 0; i <= level && i < static_cast<int>(segments.size()); i++) {
    const std::string& segment = segments[static_cast<size_t>(i)];
    PivotNode* found = nullptr;

    // Look for existing child with matching hierarchy value at this level
    for (auto& child : current->children) {
      if (static_cast<size_t>(i) < child->hierarchy_values.size() &&
          child->hierarchy_values[static_cast<size_t>(i)] == segment) {
        found = child.get();
        break;
      }
    }

    if (!found) {
      auto node = std::make_unique<PivotNode>();
      node->id = (*next_id)++;
      node->level = i;
      node->parent = current;

      // Store hierarchy values (values up to level i, rest empty for NULL)
      node->hierarchy_values.resize(num_hier);
      for (int j = 0; j <= i && j < static_cast<int>(segments.size()); j++) {
        node->hierarchy_values[static_cast<size_t>(j)] =
            segments[static_cast<size_t>(j)];
      }

      found = node.get();
      current->children.push_back(std::move(node));
    }
    current = found;
  }
  return current;
}

// Gets the display name for a node (the hierarchy value at its level).
std::string GetNodeName(const PivotNode* node) {
  if (node->level < 0 ||
      static_cast<size_t>(node->level) >= node->hierarchy_values.size()) {
    return "";
  }
  return node->hierarchy_values[static_cast<size_t>(node->level)];
}

// Sorts children of all nodes using the given sort spec.
void SortTree(PivotNode* node, const PivotSortSpec& spec) {
  if (!node) {
    return;
  }

  std::sort(node->children.begin(), node->children.end(),
            [&spec](const std::unique_ptr<PivotNode>& a,
                    const std::unique_ptr<PivotNode>& b) {
              if (spec.agg_index < 0) {
                // Sort by name (hierarchy value at node's level)
                std::string name_a = GetNodeName(a.get());
                std::string name_b = GetNodeName(b.get());
                return spec.descending ? (name_a > name_b) : (name_a < name_b);
              }
              size_t idx = static_cast<size_t>(spec.agg_index);
              if (idx >= a->aggs.size() || idx >= b->aggs.size()) {
                return false;
              }
              const PivotValue& val_a = a->aggs[idx];
              const PivotValue& val_b = b->aggs[idx];

              // Handle string comparison for MIN/MAX of text
              if (std::holds_alternative<std::string>(val_a) &&
                  std::holds_alternative<std::string>(val_b)) {
                const std::string& str_a = std::get<std::string>(val_a);
                const std::string& str_b = std::get<std::string>(val_b);
                return spec.descending ? (str_a > str_b) : (str_a < str_b);
              }

              // For numeric types, convert to double
              double d_a = PivotValueToDouble(val_a);
              double d_b = PivotValueToDouble(val_b);
              return spec.descending ? (d_a > d_b) : (d_a < d_b);
            });

  for (auto& child : node->children) {
    SortTree(child.get(), spec);
  }
}

// Flattens the tree into a vector of visible nodes.
// Only shows children of nodes that are expanded (or root's children always).
// All nodes are collapsed by default - only expanded IDs show their children.
void FlattenTree(PivotNode* node,
                 const base::FlatHashMap<int64_t, bool>& expansion_ids,
                 bool denylist_mode,
                 std::vector<PivotNode*>* out) {
  if (!node) {
    return;
  }

  // Root (level -1) is always "expanded" to show top-level nodes.
  // In allowlist mode: nodes are expanded if their ID is in expansion_ids.
  // In denylist mode: nodes are expanded unless their ID is in expansion_ids.
  bool in_list = (expansion_ids.Find(node->id) != nullptr);
  bool is_expanded = (node->level < 0) || (denylist_mode ? !in_list : in_list);
  node->expanded = is_expanded;

  // Add children to output if this node is expanded
  if (is_expanded) {
    for (auto& child : node->children) {
      out->push_back(child.get());
      // Recursively add grandchildren if child is also expanded
      FlattenTree(child.get(), expansion_ids, denylist_mode, out);
    }
  }
}

// Parses a sort specification string like "agg_0 DESC".
PivotSortSpec ParseSortSpec(const std::string& sort_str) {
  PivotSortSpec spec;
  spec.agg_index = 0;  // Default: sort by first aggregate
  spec.descending = true;

  std::string lower = base::ToLower(sort_str);
  if (lower.find("asc") != std::string::npos) {
    spec.descending = false;
  }
  if (lower.find("name") != std::string::npos ||
      lower.find("__name") != std::string::npos) {
    spec.agg_index = -1;  // Sort by name
  }

  // Try to extract aggregate index from "agg_N" pattern
  size_t agg_pos = lower.find("agg_");
  if (agg_pos != std::string::npos) {
    // Extract only the digits after "agg_"
    size_t start = agg_pos + 4;
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

// Builds the tree from the base table.
// aggregations contains full aggregation expressions like "SUM(col)",
// "COUNT(*)", etc.
base::Status BuildTree(PerfettoSqlEngine* engine,
                       const std::string& base_table,
                       const std::vector<std::string>& hierarchy_cols,
                       const std::vector<std::string>& aggregations,
                       PivotNode* root,
                       int* total_nodes) {
  // Build the aggregation query using UNION ALL (SQLite doesn't support ROLLUP)
  // We create one query per aggregation level and union them together.
  std::string query;

  size_t num_hier = hierarchy_cols.size();
  size_t num_aggs = aggregations.size();

  // Grand total query (level -1): all hierarchy cols are NULL
  query += "SELECT ";
  for (size_t i = 0; i < num_hier; i++) {
    if (i > 0) {
      query += ", ";
    }
    query += "NULL AS " + hierarchy_cols[i];
  }
  for (size_t i = 0; i < num_aggs; i++) {
    query += ", " + aggregations[i] + " AS agg_" + std::to_string(i);
  }
  query += " FROM " + base_table;

  // One query per hierarchy level
  for (size_t level = 0; level < num_hier; level++) {
    query += " UNION ALL SELECT ";

    // Columns up to and including this level are real, rest are NULL
    for (size_t i = 0; i < num_hier; i++) {
      if (i > 0) {
        query += ", ";
      }
      if (i <= level) {
        query += hierarchy_cols[i];
      } else {
        query += "NULL AS " + hierarchy_cols[i];
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

  // Initialize root node (ID 0, level -1)
  root->id = 0;
  root->level = -1;
  root->hierarchy_values.resize(num_hier);
  root->aggs.resize(num_aggs, std::monostate{});

  // Next ID to assign (root is 0, children start at 1)
  int64_t next_id = 1;
  *total_nodes = 0;

  // Helper lambda to process a single row from the statement.
  auto process_row = [&]() {
    // Determine level by counting non-NULL hierarchy columns
    int level = -1;
    std::vector<std::string> segments;

    for (size_t i = 0; i < num_hier; i++) {
      if (sqlite3_column_type(stmt.sqlite_stmt(), static_cast<int>(i)) !=
          SQLITE_NULL) {
        level = static_cast<int>(i);
        const char* val = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt.sqlite_stmt(), static_cast<int>(i)));
        segments.push_back(val ? val : "");
      } else {
        break;
      }
    }

    // Get aggregate values (type-aware)
    std::vector<PivotValue> aggs;
    for (size_t i = 0; i < num_aggs; i++) {
      int col_idx = static_cast<int>(num_hier + i);
      int sql_type = sqlite3_column_type(stmt.sqlite_stmt(), col_idx);
      PivotValue val;

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
      root->aggs = std::move(aggs);
    } else {
      // Find or create node and store aggregates
      PivotNode* node =
          FindOrCreateNode(root, segments, level, num_hier, &next_id);
      if (node) {
        node->aggs = std::move(aggs);
        (*total_nodes)++;
      }
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

int PivotOperatorModule::Create(sqlite3* db,
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
        "__intrinsic_pivot requires 3 arguments: base_table, hierarchy_cols, "
        "aggregations");
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
  res->hierarchy_cols = std::move(hierarchy_cols);
  res->aggregations = std::move(aggregations);
  res->agg_col_count = res->aggregations.size();
  // Column layout: hierarchy cols + 5 metadata + agg cols + 6 hidden
  size_t num_hier = res->hierarchy_cols.size();
  res->total_col_count =
      static_cast<int>(num_hier + kMetadataColCount + res->agg_col_count + 6);

  // Create root node
  res->root = std::make_unique<PivotNode>();

  // Build the tree from base table
  base::Status status =
      BuildTree(ctx->engine, res->base_table, res->hierarchy_cols,
                res->aggregations, res->root.get(), &res->total_nodes);
  if (!status.ok()) {
    *pzErr = sqlite3_mprintf("%s", status.c_message());
    return SQLITE_ERROR;
  }

  // Initial sort by first aggregate descending
  PivotSortSpec default_sort;
  default_sort.agg_index = 0;
  default_sort.descending = true;
  SortTree(res->root.get(), default_sort);
  res->current_sort_spec = "agg_0 DESC";

  *vtab = res.release();
  return SQLITE_OK;
}

int PivotOperatorModule::Destroy(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> tab(GetVtab(vtab));
  return SQLITE_OK;
}

int PivotOperatorModule::Connect(sqlite3* db,
                                 void* raw_ctx,
                                 int argc,
                                 const char* const* argv,
                                 sqlite3_vtab** vtab,
                                 char** pzErr) {
  return Create(db, raw_ctx, argc, argv, vtab, pzErr);
}

int PivotOperatorModule::Disconnect(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> tab(GetVtab(vtab));
  return SQLITE_OK;
}

int PivotOperatorModule::BestIndex(sqlite3_vtab* vtab,
                                   sqlite3_index_info* info) {
  auto* t = GetVtab(vtab);

  // Calculate the column indices for hidden columns
  // Layout: hierarchy cols + metadata cols + aggregate cols + hidden cols
  int num_hier = static_cast<int>(t->hierarchy_cols.size());
  int hidden_start =
      num_hier + kMetadataColCount + static_cast<int>(t->agg_col_count);
  int aggs_col = hidden_start + kAggsSpec;
  int expanded_col = hidden_start + kExpandedIds;
  int collapsed_col = hidden_start + kCollapsedIds;
  int sort_col = hidden_start + kSortSpec;
  int offset_col = hidden_start + kOffset;
  int limit_col = hidden_start + kLimit;

  // Build idxStr to encode argv index for each constraint type.
  // Format: 6 characters, one per constraint type (aggs, expanded, collapsed,
  // sort, offset, limit). Each char is '0'-'5' indicating the argv index,
  // or '-' if not present.
  // This allows Filter() to know exactly which argv slot each value is in.
  char idx_flags[7] = "------";

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
    }
  }

  info->idxStr = sqlite3_mprintf("%s", idx_flags);
  info->needToFreeIdxStr = true;
  info->estimatedCost = 1000.0;

  return SQLITE_OK;
}

int PivotOperatorModule::Open(sqlite3_vtab*, sqlite3_vtab_cursor** cursor) {
  auto c = std::make_unique<Cursor>();
  *cursor = c.release();
  return SQLITE_OK;
}

int PivotOperatorModule::Close(sqlite3_vtab_cursor* cursor) {
  delete GetCursor(cursor);
  return SQLITE_OK;
}

int PivotOperatorModule::Filter(sqlite3_vtab_cursor* cursor,
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

  // Map of expanded/collapsed node IDs and mode
  base::FlatHashMap<int64_t, bool> expansion_ids;
  bool denylist_mode = false;        // false = allowlist (expanded_ids), true =
                                     // denylist (collapsed_ids)
  bool expansion_specified = false;  // Track if any expansion constraint given

  // Parse idxStr to determine which arguments are present and their argv index.
  // Each char in idxStr is either '-' (not present) or '0'-'5' (argv index).
  std::string flags = idxStr ? idxStr : "------";

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
            expansion_ids.Insert(*id, true);
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
    denylist_mode = false;
    expansion_specified = true;
  }

  // Process __collapsed_ids (flag position 2) - denylist mode (expand all
  // except). Note: If both expanded_ids and collapsed_ids are provided,
  // collapsed_ids wins
  if (sqlite3_value* val = get_argv(2)) {
    const char* ids_str =
        reinterpret_cast<const char*>(sqlite3_value_text(val));
    expansion_ids.Clear();
    parse_ids(ids_str);
    denylist_mode = true;
    expansion_specified = true;
  }

  // Default: expand all groups when no expansion constraint is specified
  if (!expansion_specified) {
    denylist_mode = true;  // Denylist with empty set = expand all
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
    c->offset = sqlite3_value_int(val);
  }

  // Process __limit (flag position 5)
  if (sqlite3_value* val = get_argv(5)) {
    c->limit = sqlite3_value_int(val);
  }

  // Resort if sort spec changed or if we haven't sorted yet
  // Default to "agg_0 DESC" if no sort spec provided
  if (sort_spec_str.empty()) {
    sort_spec_str = "agg_0 DESC";
  }
  if (sort_spec_str != t->current_sort_spec) {
    PivotSortSpec spec = ParseSortSpec(sort_spec_str);
    SortTree(t->root.get(), spec);
    t->current_sort_spec = sort_spec_str;
  }

  // Flatten the tree based on expansion state
  t->flat.clear();
  FlattenTree(t->root.get(), expansion_ids, denylist_mode, &t->flat);

  // Apply offset
  c->row_index = c->offset;

  return SQLITE_OK;
}

int PivotOperatorModule::Next(sqlite3_vtab_cursor* cursor) {
  auto* c = GetCursor(cursor);
  c->row_index++;
  c->rows_returned++;
  return SQLITE_OK;
}

int PivotOperatorModule::Eof(sqlite3_vtab_cursor* cursor) {
  auto* t = GetVtab(cursor->pVtab);
  auto* c = GetCursor(cursor);

  if (c->rows_returned >= c->limit) {
    return 1;
  }
  if (c->row_index >= static_cast<int>(t->flat.size())) {
    return 1;
  }
  return 0;
}

int PivotOperatorModule::Column(sqlite3_vtab_cursor* cursor,
                                sqlite3_context* ctx,
                                int col) {
  auto* t = GetVtab(cursor->pVtab);
  auto* c = GetCursor(cursor);

  if (c->row_index >= static_cast<int>(t->flat.size())) {
    sqlite::result::Null(ctx);
    return SQLITE_OK;
  }

  PivotNode* node = t->flat[static_cast<size_t>(c->row_index)];
  int num_hier = static_cast<int>(t->hierarchy_cols.size());

  // Column layout:
  // [0..num_hier-1]: hierarchy columns (with NULLs like ROLLUP)
  // [num_hier+0]: __id
  // [num_hier+1]: __parent_id
  // [num_hier+2]: __depth
  // [num_hier+3]: __has_children
  // [num_hier+4]: __child_count
  // [num_hier+5..]: agg_0, agg_1, ...

  if (col < num_hier) {
    // Hierarchy column - return value if level >= col, else NULL
    size_t hier_idx = static_cast<size_t>(col);
    if (node->level >= col && hier_idx < node->hierarchy_values.size() &&
        !node->hierarchy_values[hier_idx].empty()) {
      const std::string& val = node->hierarchy_values[hier_idx];
      sqlite::result::StaticString(ctx, val.c_str(),
                                   static_cast<int>(val.size()));
    } else {
      sqlite::result::Null(ctx);
    }
  } else if (col == num_hier + kIdOffset) {
    sqlite::result::Long(ctx, node->id);
  } else if (col == num_hier + kParentIdOffset) {
    if (node->parent) {
      sqlite::result::Long(ctx, node->parent->id);
    } else {
      sqlite::result::Null(ctx);
    }
  } else if (col == num_hier + kDepthOffset) {
    sqlite::result::Long(ctx, node->level);
  } else if (col == num_hier + kHasChildrenOffset) {
    sqlite::result::Long(ctx, node->children.empty() ? 0 : 1);
  } else if (col == num_hier + kChildCountOffset) {
    sqlite::result::Long(ctx, static_cast<int64_t>(node->children.size()));
  } else {
    // Aggregate or hidden column
    int agg_start = num_hier + kMetadataColCount;
    int agg_end = agg_start + static_cast<int>(t->agg_col_count);
    if (col >= agg_start && col < agg_end) {
      size_t agg_idx = static_cast<size_t>(col - agg_start);
      if (agg_idx < node->aggs.size()) {
        const PivotValue& val = node->aggs[agg_idx];
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

int PivotOperatorModule::Rowid(sqlite3_vtab_cursor* cursor,
                               sqlite_int64* rowid) {
  auto* c = GetCursor(cursor);
  *rowid = c->row_index;
  return SQLITE_OK;
}

}  // namespace perfetto::trace_processor
