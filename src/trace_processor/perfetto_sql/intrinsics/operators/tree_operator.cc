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

// __intrinsic_tree virtual table for hierarchical tree display.
//
// This operator displays data from a table with id/parent_id relationships
// as a tree structure with expand/collapse support.
//
// CREATION:
//   CREATE VIRTUAL TABLE my_tree USING __intrinsic_tree(
//       'source_table_or_subquery',  -- Table name or (SELECT ...) subquery
//       'id_column',                 -- Column containing the row's unique ID
//       'parent_id_column'           -- Column containing parent ID (NULL=root)
//   );
//
// QUERYING (allowlist mode - only specified IDs expanded):
//   SELECT * FROM my_tree
//   WHERE __expanded_ids__ = '1,2,3'   -- Comma-separated node IDs to expand
//     AND __sort__ = 'name ASC'        -- Optional: sort by column
//     AND __offset__ = 0               -- Optional: pagination offset
//     AND __limit__ = 100;             -- Optional: pagination limit
//
// QUERYING (denylist mode - all expanded except specified IDs):
//   SELECT * FROM my_tree
//   WHERE __collapsed_ids__ = '4,5'    -- Nodes to keep collapsed
//     AND __sort__ = 'size DESC';
//
// OUTPUT COLUMNS:
//   - All columns from the source table (in original order)
//   - __depth__: Tree depth (0 for root-level nodes)
//   - __has_children__: 1 if node has children, 0 otherwise
//   - __child_count__: Number of direct children
//
// BEHAVIOR:
//   - Nodes whose parent_id references a non-existent row become root nodes
//   - This allows filtered data to display correctly (orphans promoted to root)
//   - Tree is built once at CREATE time and cached

#include "src/trace_processor/perfetto_sql/intrinsics/operators/tree_operator.h"

#include <sqlite3.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {

namespace {

// Remove surrounding quotes from a string if present.
std::string RemoveQuotes(const std::string& s) {
  if (s.size() >= 2 && ((s.front() == '\'' && s.back() == '\'') ||
                        (s.front() == '"' && s.back() == '"'))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

// Builds the schema declaration string for the virtual table.
std::string BuildSchemaString(const std::vector<std::string>& column_names,
                              const std::vector<std::string>& column_types) {
  std::string schema = "CREATE TABLE x(";

  // Source table columns first
  for (size_t i = 0; i < column_names.size(); i++) {
    if (i > 0) {
      schema += ",";
    }
    schema += column_names[i];
    if (i < column_types.size() && !column_types[i].empty()) {
      schema += " " + column_types[i];
    }
  }

  // Metadata columns
  schema += ",__depth__ INTEGER";
  schema += ",__has_children__ INTEGER";
  schema += ",__child_count__ INTEGER";

  // Hidden columns for query parameters
  schema += ",__expanded_ids__ TEXT HIDDEN";
  schema += ",__collapsed_ids__ TEXT HIDDEN";
  schema += ",__sort__ TEXT HIDDEN";
  schema += ",__offset__ INTEGER HIDDEN";
  schema += ",__limit__ INTEGER HIDDEN";

  schema += ")";
  return schema;
}

// Convert a TreeValue to a sortable double for comparison.
// Strings are hashed for numeric comparison.
double TreeValueToDouble(const TreeValue& val) {
  if (std::holds_alternative<std::monostate>(val)) {
    return std::numeric_limits<double>::lowest();
  }
  if (std::holds_alternative<int64_t>(val)) {
    return static_cast<double>(std::get<int64_t>(val));
  }
  if (std::holds_alternative<double>(val)) {
    return std::get<double>(val);
  }
  // For strings, return a simple hash for comparison
  // This isn't perfect but allows basic sorting
  const auto& s = std::get<std::string>(val);
  double hash = 0;
  for (size_t i = 0; i < std::min(s.size(), size_t{8}); i++) {
    hash = hash * 256 + static_cast<unsigned char>(s[i]);
  }
  return hash;
}

// Sort children of all nodes using the given sort spec.
void SortTree(std::vector<std::unique_ptr<TreeNode>>& nodes,
              const TreeSortSpec& spec) {
  if (spec.col_index < 0) {
    return;
  }

  std::sort(nodes.begin(), nodes.end(),
            [&spec](const std::unique_ptr<TreeNode>& a,
                    const std::unique_ptr<TreeNode>& b) {
              size_t idx = static_cast<size_t>(spec.col_index);
              if (idx >= a->values.size() || idx >= b->values.size()) {
                return false;
              }

              // For strings, compare lexicographically
              const TreeValue& val_a = a->values[idx];
              const TreeValue& val_b = b->values[idx];

              // Handle NULL (monostate) - NULLs sort last
              bool a_null = std::holds_alternative<std::monostate>(val_a);
              bool b_null = std::holds_alternative<std::monostate>(val_b);
              if (a_null && b_null)
                return false;
              if (a_null)
                return spec.descending;  // NULLs at end
              if (b_null)
                return !spec.descending;

              // If both are strings, compare as strings
              if (std::holds_alternative<std::string>(val_a) &&
                  std::holds_alternative<std::string>(val_b)) {
                const auto& str_a = std::get<std::string>(val_a);
                const auto& str_b = std::get<std::string>(val_b);
                return spec.descending ? (str_a > str_b) : (str_a < str_b);
              }

              // Otherwise compare as doubles
              double d_a = TreeValueToDouble(val_a);
              double d_b = TreeValueToDouble(val_b);
              return spec.descending ? (d_a > d_b) : (d_a < d_b);
            });

  // Recursively sort children
  for (auto& node : nodes) {
    SortTree(node->children, spec);
  }
}

// Flattens the tree into a vector of visible nodes.
// Only shows children of nodes that are expanded.
void FlattenTree(TreeNode* node,
                 const std::unordered_set<int64_t>& expansion_ids,
                 bool denylist_mode,
                 std::vector<TreeNode*>* out) {
  if (!node) {
    return;
  }

  // In allowlist mode: nodes are expanded if their ID is in expansion_ids.
  // In denylist mode: nodes are expanded unless their ID is in expansion_ids.
  // Nodes without a valid ID are never expanded.
  bool in_list = node->id.has_value() && (expansion_ids.count(*node->id) > 0);
  bool is_expanded = denylist_mode ? !in_list : in_list;
  node->expanded = is_expanded;

  // Add children to output if this node is expanded
  if (is_expanded) {
    for (auto& child : node->children) {
      out->push_back(child.get());
      // Recursively add grandchildren
      FlattenTree(child.get(), expansion_ids, denylist_mode, out);
    }
  }
}

// Flatten root-level nodes (these are always visible)
void FlattenRoots(std::vector<std::unique_ptr<TreeNode>>& roots,
                  const std::unordered_set<int64_t>& expansion_ids,
                  bool denylist_mode,
                  std::vector<TreeNode*>* out) {
  for (auto& root : roots) {
    out->push_back(root.get());
    FlattenTree(root.get(), expansion_ids, denylist_mode, out);
  }
}

// Parses a sort specification string like "column_name DESC".
TreeSortSpec ParseSortSpec(const std::string& sort_str,
                           const std::vector<std::string>& column_names) {
  TreeSortSpec spec;
  spec.col_index = -1;
  spec.descending = true;

  std::string lower = base::ToLower(sort_str);
  if (lower.find("asc") != std::string::npos) {
    spec.descending = false;
  }

  // Find column name - extract everything before ASC/DESC
  std::string col_name = sort_str;
  size_t asc_pos = lower.find(" asc");
  size_t desc_pos = lower.find(" desc");
  if (asc_pos != std::string::npos) {
    col_name = sort_str.substr(0, asc_pos);
  } else if (desc_pos != std::string::npos) {
    col_name = sort_str.substr(0, desc_pos);
  }
  col_name = base::TrimWhitespace(col_name);

  // Find column index
  for (size_t i = 0; i < column_names.size(); i++) {
    if (base::ToLower(column_names[i]) == base::ToLower(col_name)) {
      spec.col_index = static_cast<int>(i);
      break;
    }
  }

  return spec;
}

// Query the source table schema to get column names and types.
base::Status GetSourceSchema(PerfettoSqlEngine* engine,
                             const std::string& source_table,
                             std::vector<std::string>* column_names,
                             std::vector<std::string>* column_types) {
  // Use SELECT * LIMIT 0 to get column names - works for all table types
  // including PERFETTO TABLEs which don't respond to PRAGMA table_info
  std::string trimmed = base::TrimWhitespace(source_table);
  bool is_subquery =
      !trimmed.empty() &&
      (trimmed[0] == '(' || base::ToLower(trimmed).find("select") == 0);

  std::string query;
  if (is_subquery) {
    query = "SELECT * FROM " + trimmed + " AS __schema_query__ LIMIT 0";
  } else {
    query = "SELECT * FROM " + source_table + " LIMIT 0";
  }

  auto result = engine->ExecuteUntilLastStatement(
      SqlSource::FromTraceProcessorImplementation(query));
  if (!result.ok()) {
    return result.status();
  }

  auto& stmt = result->stmt;

  // Get column info from statement
  int col_count = sqlite3_column_count(stmt.sqlite_stmt());
  for (int i = 0; i < col_count; i++) {
    const char* name = sqlite3_column_name(stmt.sqlite_stmt(), i);
    column_names->push_back(name ? name : ("col_" + std::to_string(i)));
    // Type information is not reliably available, leave empty
    column_types->push_back("");
  }

  if (column_names->empty()) {
    return base::ErrStatus("Could not determine schema for source table");
  }

  return base::OkStatus();
}

// Build the tree from the source table using id/parent_id relationships.
base::Status BuildTree(PerfettoSqlEngine* engine,
                       const std::string& source_table,
                       const std::vector<std::string>& column_names,
                       int id_col_index,
                       int parent_id_col_index,
                       std::vector<std::unique_ptr<TreeNode>>* roots,
                       int* total_nodes) {
  // Query all rows from the source table
  std::string query = "SELECT * FROM " + source_table;

  auto result = engine->ExecuteUntilLastStatement(
      SqlSource::FromTraceProcessorImplementation(query));
  if (!result.ok()) {
    return result.status();
  }

  auto& stmt = result->stmt;
  int col_count = static_cast<int>(column_names.size());

  // First pass: create all nodes and store in a map by ID
  // Using std::unordered_map because we need to look up parent pointers by ID.
  std::unordered_map<int64_t, TreeNode*> node_map;
  std::vector<std::unique_ptr<TreeNode>> all_nodes;

  // Helper lambda to process a single row from the statement.
  // ExecuteUntilLastStatement steps once, so the first row is already
  // available.
  auto process_row = [&]() {
    auto node = std::make_unique<TreeNode>();

    // Read all column values
    for (int i = 0; i < col_count; i++) {
      int sql_type = sqlite3_column_type(stmt.sqlite_stmt(), i);
      TreeValue val;

      switch (sql_type) {
        case SQLITE_INTEGER:
          val = sqlite3_column_int64(stmt.sqlite_stmt(), i);
          break;
        case SQLITE_FLOAT:
          val = sqlite3_column_double(stmt.sqlite_stmt(), i);
          break;
        case SQLITE_TEXT: {
          const char* text = reinterpret_cast<const char*>(
              sqlite3_column_text(stmt.sqlite_stmt(), i));
          val = std::string(text ? text : "");
          break;
        }
        case SQLITE_BLOB: {
          const void* blob = sqlite3_column_blob(stmt.sqlite_stmt(), i);
          int size = sqlite3_column_bytes(stmt.sqlite_stmt(), i);
          val = std::string(static_cast<const char*>(blob),
                            static_cast<size_t>(size));
          break;
        }
        case SQLITE_NULL:
        default:
          val = std::monostate{};
          break;
      }
      node->values.push_back(std::move(val));
    }

    // Extract ID from the id column
    if (id_col_index >= 0 &&
        id_col_index < static_cast<int>(node->values.size())) {
      const TreeValue& id_val = node->values[static_cast<size_t>(id_col_index)];
      if (std::holds_alternative<int64_t>(id_val)) {
        node->id = std::get<int64_t>(id_val);
      }
      // If NULL or not an integer, leave id as std::nullopt
    }

    // Extract parent ID from the parent_id column
    if (parent_id_col_index >= 0 &&
        parent_id_col_index < static_cast<int>(node->values.size())) {
      const TreeValue& parent_val =
          node->values[static_cast<size_t>(parent_id_col_index)];
      if (std::holds_alternative<int64_t>(parent_val)) {
        node->parent_id = std::get<int64_t>(parent_val);
      }
      // If NULL, leave parent_id as std::nullopt (root node)
    }

    TreeNode* raw_ptr = node.get();
    // Only add to map if node has a valid ID
    if (node->id.has_value()) {
      node_map[*node->id] = raw_ptr;
    }
    all_nodes.push_back(std::move(node));
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

  // Second pass: build tree structure
  for (auto& node : all_nodes) {
    if (node->parent_id.has_value()) {
      auto it = node_map.find(*node->parent_id);
      if (it != node_map.end()) {
        TreeNode* parent = it->second;
        node->parent = parent;
        node->depth = parent->depth + 1;
        parent->children.push_back(std::move(node));
      } else {
        // Parent not found - treat as root
        roots->push_back(std::move(node));
      }
    } else {
      // No parent - this is a root node
      roots->push_back(std::move(node));
    }
  }

  *total_nodes = static_cast<int>(node_map.size());
  return base::OkStatus();
}

}  // namespace

int TreeOperatorModule::Create(sqlite3* db,
                               void* raw_ctx,
                               int argc,
                               const char* const* argv,
                               sqlite3_vtab** vtab,
                               char** pzErr) {
  // argv[0] = module name
  // argv[1] = database name
  // argv[2] = table name
  // argv[3] = source table or subquery
  // argv[4] = id column name
  // argv[5] = parent_id column name

  if (argc < 6) {
    *pzErr = sqlite3_mprintf(
        "__intrinsic_tree requires 3 arguments: source_table, id_column, "
        "parent_id_column");
    return SQLITE_ERROR;
  }

  auto* ctx = GetContext(raw_ctx);

  std::string source_table = RemoveQuotes(argv[3]);
  std::string id_column = RemoveQuotes(argv[4]);
  std::string parent_id_column = RemoveQuotes(argv[5]);

  // Get schema from source table
  std::vector<std::string> column_names;
  std::vector<std::string> column_types;
  base::Status status =
      GetSourceSchema(ctx->engine, source_table, &column_names, &column_types);
  if (!status.ok()) {
    *pzErr = sqlite3_mprintf("%s", status.c_message());
    return SQLITE_ERROR;
  }

  // Find id and parent_id column indices
  int id_col_index = -1;
  int parent_id_col_index = -1;
  for (size_t i = 0; i < column_names.size(); i++) {
    if (column_names[i] == id_column) {
      id_col_index = static_cast<int>(i);
    }
    if (column_names[i] == parent_id_column) {
      parent_id_col_index = static_cast<int>(i);
    }
  }

  if (id_col_index < 0) {
    *pzErr = sqlite3_mprintf("ID column '%s' not found in source table",
                             id_column.c_str());
    return SQLITE_ERROR;
  }
  if (parent_id_col_index < 0) {
    *pzErr = sqlite3_mprintf("Parent ID column '%s' not found in source table",
                             parent_id_column.c_str());
    return SQLITE_ERROR;
  }

  // Build and declare schema
  std::string schema = BuildSchemaString(column_names, column_types);
  if (int ret = sqlite3_declare_vtab(db, schema.c_str()); ret != SQLITE_OK) {
    return ret;
  }

  // Create the vtab
  auto res = std::make_unique<Vtab>();
  res->engine = ctx->engine;
  res->base_table = std::move(source_table);
  res->id_column = std::move(id_column);
  res->parent_id_column = std::move(parent_id_column);
  res->column_names = std::move(column_names);
  res->column_types = std::move(column_types);
  res->id_col_index = id_col_index;
  res->parent_id_col_index = parent_id_col_index;

  // Column layout: source cols + 3 metadata + 7 hidden
  int num_source_cols = static_cast<int>(res->column_names.size());
  res->total_col_count = num_source_cols + kMetadataColCount + kHiddenColCount;

  // Build the tree from source table
  status = BuildTree(ctx->engine, res->base_table, res->column_names,
                     res->id_col_index, res->parent_id_col_index, &res->roots,
                     &res->total_nodes);
  if (!status.ok()) {
    *pzErr = sqlite3_mprintf("%s", status.c_message());
    return SQLITE_ERROR;
  }

  *vtab = res.release();
  return SQLITE_OK;
}

int TreeOperatorModule::Destroy(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> tab(GetVtab(vtab));
  return SQLITE_OK;
}

int TreeOperatorModule::Connect(sqlite3* db,
                                void* raw_ctx,
                                int argc,
                                const char* const* argv,
                                sqlite3_vtab** vtab,
                                char** pzErr) {
  return Create(db, raw_ctx, argc, argv, vtab, pzErr);
}

int TreeOperatorModule::Disconnect(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> tab(GetVtab(vtab));
  return SQLITE_OK;
}

int TreeOperatorModule::BestIndex(sqlite3_vtab* vtab,
                                  sqlite3_index_info* info) {
  auto* t = GetVtab(vtab);

  // Calculate the column indices for hidden columns
  // Layout: source cols + metadata cols + hidden cols
  int num_source_cols = static_cast<int>(t->column_names.size());
  int hidden_start = num_source_cols + kMetadataColCount;
  int expanded_col = hidden_start + kExpandedIds;
  int collapsed_col = hidden_start + kCollapsedIds;
  int sort_col = hidden_start + kSortSpec;
  int offset_col = hidden_start + kOffset;
  int limit_col = hidden_start + kLimit;

  // Build idxStr to encode argv index for each constraint type.
  // Format: 5 characters (expanded, collapsed, sort, offset, limit).
  // Each char is '0'-'4' indicating the argv index, or '-' if not present.
  char idx_flags[6] = "-----";

  int argv_index = 1;  // argvIndex is 1-based in SQLite
  for (int i = 0; i < info->nConstraint; i++) {
    if (!info->aConstraint[i].usable) {
      continue;
    }
    if (!sqlite::utils::IsOpEq(info->aConstraint[i].op)) {
      continue;
    }

    int col = info->aConstraint[i].iColumn;
    if (col == expanded_col) {
      idx_flags[0] = static_cast<char>('0' + argv_index - 1);
      info->aConstraintUsage[i].argvIndex = argv_index++;
      info->aConstraintUsage[i].omit = true;
    } else if (col == collapsed_col) {
      idx_flags[1] = static_cast<char>('0' + argv_index - 1);
      info->aConstraintUsage[i].argvIndex = argv_index++;
      info->aConstraintUsage[i].omit = true;
    } else if (col == sort_col) {
      idx_flags[2] = static_cast<char>('0' + argv_index - 1);
      info->aConstraintUsage[i].argvIndex = argv_index++;
      info->aConstraintUsage[i].omit = true;
    } else if (col == offset_col) {
      idx_flags[3] = static_cast<char>('0' + argv_index - 1);
      info->aConstraintUsage[i].argvIndex = argv_index++;
      info->aConstraintUsage[i].omit = true;
    } else if (col == limit_col) {
      idx_flags[4] = static_cast<char>('0' + argv_index - 1);
      info->aConstraintUsage[i].argvIndex = argv_index++;
      info->aConstraintUsage[i].omit = true;
    }
  }

  info->idxStr = sqlite3_mprintf("%s", idx_flags);
  info->needToFreeIdxStr = true;
  info->estimatedCost = 1000.0;

  return SQLITE_OK;
}

int TreeOperatorModule::Open(sqlite3_vtab*, sqlite3_vtab_cursor** cursor) {
  auto c = std::make_unique<Cursor>();
  *cursor = c.release();
  return SQLITE_OK;
}

int TreeOperatorModule::Close(sqlite3_vtab_cursor* cursor) {
  delete GetCursor(cursor);
  return SQLITE_OK;
}

int TreeOperatorModule::Filter(sqlite3_vtab_cursor* cursor,
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

  // Set of expanded/collapsed node IDs and mode
  std::unordered_set<int64_t> expansion_ids;
  bool denylist_mode = false;

  // Parse idxStr to determine which arguments are present
  std::string flags = idxStr ? idxStr : "-----";

  std::string sort_spec_str;

  // Helper to get argv value for a flag position
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
            expansion_ids.insert(*id);
          }
        }
      }
    }
  };

  // Process __expanded_ids__ (flag position 0) - allowlist mode
  if (sqlite3_value* val = get_argv(0)) {
    const char* ids_str =
        reinterpret_cast<const char*>(sqlite3_value_text(val));
    parse_ids(ids_str);
    denylist_mode = false;
  }

  // Process __collapsed_ids__ (flag position 1) - denylist mode
  // Note: If both are provided, collapsed_ids wins
  if (sqlite3_value* val = get_argv(1)) {
    const char* ids_str =
        reinterpret_cast<const char*>(sqlite3_value_text(val));
    expansion_ids.clear();
    parse_ids(ids_str);
    denylist_mode = true;
  }

  // Process __sort__ (flag position 2)
  if (sqlite3_value* val = get_argv(2)) {
    const char* sort_str =
        reinterpret_cast<const char*>(sqlite3_value_text(val));
    if (sort_str) {
      sort_spec_str = sort_str;
    }
  }

  // Process __offset__ (flag position 3)
  if (sqlite3_value* val = get_argv(3)) {
    c->offset = sqlite3_value_int(val);
  }

  // Process __limit__ (flag position 4)
  if (sqlite3_value* val = get_argv(4)) {
    c->limit = sqlite3_value_int(val);
  }

  // Resort if sort spec changed
  if (!sort_spec_str.empty() && sort_spec_str != t->current_sort_spec) {
    TreeSortSpec spec = ParseSortSpec(sort_spec_str, t->column_names);
    SortTree(t->roots, spec);
    t->current_sort_spec = sort_spec_str;
  }

  // Flatten the tree based on expansion state
  t->flat.clear();
  FlattenRoots(t->roots, expansion_ids, denylist_mode, &t->flat);

  // Apply offset
  c->row_index = c->offset;

  return SQLITE_OK;
}

int TreeOperatorModule::Next(sqlite3_vtab_cursor* cursor) {
  auto* c = GetCursor(cursor);
  c->row_index++;
  c->rows_returned++;
  return SQLITE_OK;
}

int TreeOperatorModule::Eof(sqlite3_vtab_cursor* cursor) {
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

int TreeOperatorModule::Column(sqlite3_vtab_cursor* cursor,
                               sqlite3_context* ctx,
                               int col) {
  auto* t = GetVtab(cursor->pVtab);
  auto* c = GetCursor(cursor);

  if (c->row_index >= static_cast<int>(t->flat.size())) {
    sqlite::result::Null(ctx);
    return SQLITE_OK;
  }

  TreeNode* node = t->flat[static_cast<size_t>(c->row_index)];
  int num_source_cols = static_cast<int>(t->column_names.size());

  // Column layout:
  // [0..num_source_cols-1]: source table columns
  // [num_source_cols+0]: __depth__
  // [num_source_cols+1]: __has_children__
  // [num_source_cols+2]: __child_count__
  // [after metadata]: hidden columns

  if (col < num_source_cols) {
    // Source column
    size_t val_idx = static_cast<size_t>(col);
    if (val_idx < node->values.size()) {
      const TreeValue& val = node->values[val_idx];
      if (std::holds_alternative<std::monostate>(val)) {
        sqlite::result::Null(ctx);
      } else if (std::holds_alternative<int64_t>(val)) {
        sqlite::result::Long(ctx, std::get<int64_t>(val));
      } else if (std::holds_alternative<double>(val)) {
        sqlite::result::Double(ctx, std::get<double>(val));
      } else {
        const std::string& str = std::get<std::string>(val);
        sqlite::result::StaticString(ctx, str.c_str(),
                                     static_cast<int>(str.size()));
      }
    } else {
      sqlite::result::Null(ctx);
    }
  } else if (col == num_source_cols + kDepthOffset) {
    sqlite::result::Long(ctx, node->depth);
  } else if (col == num_source_cols + kTreeHasChildrenOffset) {
    sqlite::result::Long(ctx, node->children.empty() ? 0 : 1);
  } else if (col == num_source_cols + kChildCountOffset) {
    sqlite::result::Long(ctx, static_cast<int64_t>(node->children.size()));
  } else {
    // Hidden columns - return NULL
    sqlite::result::Null(ctx);
  }

  return SQLITE_OK;
}

int TreeOperatorModule::Rowid(sqlite3_vtab_cursor* cursor,
                              sqlite_int64* rowid) {
  auto* c = GetCursor(cursor);
  *rowid = c->row_index;
  return SQLITE_OK;
}

}  // namespace perfetto::trace_processor
