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

#include "src/trace_processor/perfetto_sql/pfgraph/pfgraph_yaml_to_ast.h"

#include <cstdlib>
#include <string>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/perfetto_sql/pfgraph/pfgraph_ast.h"
#include "src/trace_processor/perfetto_sql/pfgraph/yaml_parser.h"

namespace perfetto::trace_processor::pfgraph {

namespace {

// ---------------------------------------------------------------------------
// Helper: get a required string scalar from a mapping node.
// ---------------------------------------------------------------------------
base::StatusOr<std::string> GetString(const YamlNode& node,
                                      const std::string& key,
                                      const char* context) {
  const YamlNode* child = node.find(key);
  if (!child || !child->is_scalar()) {
    return base::ErrStatus("pfgraph-yaml: missing or non-scalar '%s' in '%s'",
                           key.c_str(), context);
  }
  return std::string(child->scalar());
}

// ---------------------------------------------------------------------------
// Helper: get an optional string scalar (returns empty if missing).
// ---------------------------------------------------------------------------
std::string GetOptString(const YamlNode& node, const std::string& key) {
  const YamlNode* child = node.find(key);
  if (!child || !child->is_scalar()) {
    return "";
  }
  return child->scalar();
}

// ---------------------------------------------------------------------------
// Helper: convert a sequence of scalars into a vector<string>.
// ---------------------------------------------------------------------------
base::StatusOr<std::vector<std::string>> GetStringList(
    const YamlNode& node,
    const char* context) {
  std::vector<std::string> result;
  if (node.is_scalar()) {
    // Single value treated as one-element list.
    result.push_back(node.scalar());
    return result;
  }
  if (!node.is_sequence()) {
    return base::ErrStatus(
        "pfgraph-yaml: expected sequence for '%s'", context);
  }
  for (const auto& item : node.sequence()) {
    if (!item.is_scalar()) {
      return base::ErrStatus(
          "pfgraph-yaml: expected scalar in list for '%s'", context);
    }
    result.push_back(item.scalar());
  }
  return result;
}

// ---------------------------------------------------------------------------
// Helper: parse "expr AS alias" from a single string.
// ---------------------------------------------------------------------------
ColumnSpec ParseColumnSpec(const std::string& s) {
  ColumnSpec spec;
  // Look for " AS " (case-sensitive, matching existing convention).
  auto pos = s.find(" AS ");
  if (pos != std::string::npos) {
    spec.expr = s.substr(0, pos);
    spec.alias = s.substr(pos + 4);
  } else {
    spec.expr = s;
  }
  return spec;
}

// ---------------------------------------------------------------------------
// Helper: parse a sort spec string like "col DESC" or "col".
// ---------------------------------------------------------------------------
SortSpec ParseSortSpec(const std::string& s) {
  SortSpec spec;
  if (base::EndsWith(s, " DESC")) {
    spec.column = s.substr(0, s.size() - 5);
    spec.desc = true;
  } else if (base::EndsWith(s, " ASC")) {
    spec.column = s.substr(0, s.size() - 4);
    spec.desc = false;
  } else {
    spec.column = s;
    spec.desc = false;
  }
  return spec;
}

// ---------------------------------------------------------------------------
// Helper: parse a PipelineRef from a YAML node (string or mapping with alias).
// ---------------------------------------------------------------------------
base::StatusOr<PipelineRef> ParsePipelineRefNode(const YamlNode& node,
                                                  const char* context) {
  PipelineRef ref;
  if (node.is_scalar()) {
    // Check for "name AS alias" in scalar.
    std::string s = node.scalar();
    auto pos = s.find(" AS ");
    if (pos != std::string::npos) {
      ref.name = s.substr(0, pos);
      ref.alias = s.substr(pos + 4);
    } else {
      ref.name = s;
    }
    return ref;
  }
  if (node.is_mapping()) {
    std::string name;
    ASSIGN_OR_RETURN(name, GetString(node, "name", context));
    ref.name = std::move(name);
    ref.alias = GetOptString(node, "alias");
    return ref;
  }
  return base::ErrStatus("pfgraph-yaml: expected string or mapping for '%s'",
                         context);
}

// ---------------------------------------------------------------------------
// Forward declarations.
// ---------------------------------------------------------------------------
base::StatusOr<Source> ParseSourceNode(const YamlNode& node);
base::StatusOr<Operation> ParseOperationEntry(const std::string& key,
                                              const YamlNode& value);
base::StatusOr<Pipeline> ParsePipelineOps(const YamlNode& node);

// ---------------------------------------------------------------------------
// Source parsing.
// ---------------------------------------------------------------------------
base::StatusOr<Source> ParseSourceNode(const YamlNode& node) {
  if (node.is_scalar()) {
    // A bare string: pipeline reference.
    PipelineRef ref;
    std::string s = node.scalar();
    auto pos = s.find(" AS ");
    if (pos != std::string::npos) {
      ref.name = s.substr(0, pos);
      ref.alias = s.substr(pos + 4);
    } else {
      ref.name = s;
    }
    return Source{std::move(ref)};
  }
  if (!node.is_mapping()) {
    return base::ErrStatus(
        "pfgraph-yaml: 'from' must be a string or mapping");
  }

  // Check which source type this is.
  if (node.find("sql")) {
    SqlSource src;
    std::string sql;
    ASSIGN_OR_RETURN(sql, GetString(node, "sql", "sql source"));
    src.sql = std::move(sql);
    const YamlNode* modules = node.find("modules");
    if (modules && modules->is_sequence()) {
      std::vector<std::string> mods;
      ASSIGN_OR_RETURN(mods, GetStringList(*modules, "sql.modules"));
      src.modules = std::move(mods);
    }
    return Source{std::move(src)};
  }

  if (node.find("slices")) {
    // slices: {name: 'glob', process: 'glob', ...}
    // Or the slices key itself is a mapping.
    SlicesSource src;
    const YamlNode* slices_node = node.find("slices");
    if (slices_node->is_mapping()) {
      src.name_glob = GetOptString(*slices_node, "name");
      src.thread_glob = GetOptString(*slices_node, "thread");
      src.process_glob = GetOptString(*slices_node, "process");
      src.track_glob = GetOptString(*slices_node, "track");
    }
    return Source{std::move(src)};
  }

  if (node.find("interval_intersect")) {
    IntervalIntersectSource src;
    const YamlNode* ii_node = node.find("interval_intersect");
    if (ii_node->is_mapping()) {
      const YamlNode* inputs = ii_node->find("inputs");
      if (inputs && inputs->is_sequence()) {
        for (const auto& item : inputs->sequence()) {
          PipelineRef ref;
          ASSIGN_OR_RETURN(ref,
                           ParsePipelineRefNode(item, "interval_intersect"));
          src.inputs.push_back(std::move(ref));
        }
      }
      const YamlNode* part = ii_node->find("partition");
      if (part) {
        std::vector<std::string> cols;
        ASSIGN_OR_RETURN(cols, GetStringList(*part, "interval_intersect.partition"));
        src.partition_columns = std::move(cols);
      }
    } else if (ii_node->is_sequence()) {
      for (const auto& item : ii_node->sequence()) {
        PipelineRef ref;
        ASSIGN_OR_RETURN(ref,
                         ParsePipelineRefNode(item, "interval_intersect"));
        src.inputs.push_back(std::move(ref));
      }
      const YamlNode* part = node.find("partition");
      if (part) {
        std::vector<std::string> cols;
        ASSIGN_OR_RETURN(cols, GetStringList(*part, "interval_intersect.partition"));
        src.partition_columns = std::move(cols);
      }
    }
    return Source{std::move(src)};
  }

  if (node.find("union")) {
    UnionSource src;
    const YamlNode* u_node = node.find("union");
    if (u_node->is_sequence()) {
      for (const auto& item : u_node->sequence()) {
        PipelineRef ref;
        ASSIGN_OR_RETURN(ref, ParsePipelineRefNode(item, "union"));
        src.inputs.push_back(std::move(ref));
      }
    }
    std::string all_str = GetOptString(node, "all");
    if (!all_str.empty()) {
      src.union_all = (all_str == "true");
    }
    return Source{std::move(src)};
  }

  if (node.find("table")) {
    TableSource src;
    std::string name;
    ASSIGN_OR_RETURN(name, GetString(node, "table", "table source"));
    src.table_name = std::move(name);
    return Source{std::move(src)};
  }

  return base::ErrStatus(
      "pfgraph-yaml: unrecognized source type in 'from'");
}

// ---------------------------------------------------------------------------
// Operation parsing.
// ---------------------------------------------------------------------------
base::StatusOr<Operation> ParseOperationEntry(const std::string& key,
                                              const YamlNode& value) {
  // --- filter ---
  if (key == "filter") {
    if (!value.is_scalar()) {
      return base::ErrStatus("pfgraph-yaml: 'filter' must be a string");
    }
    FilterOp op;
    op.expr = value.scalar();
    return Operation{std::move(op)};
  }

  // --- select ---
  if (key == "select") {
    SelectOp op;
    if (value.is_scalar()) {
      op.columns.push_back(ParseColumnSpec(value.scalar()));
    } else if (value.is_sequence()) {
      for (const auto& item : value.sequence()) {
        if (!item.is_scalar()) {
          return base::ErrStatus(
              "pfgraph-yaml: 'select' items must be strings");
        }
        op.columns.push_back(ParseColumnSpec(item.scalar()));
      }
    } else {
      return base::ErrStatus(
          "pfgraph-yaml: 'select' must be a string or list");
    }
    return Operation{std::move(op)};
  }

  // --- computed ---
  if (key == "computed") {
    if (!value.is_mapping()) {
      return base::ErrStatus("pfgraph-yaml: 'computed' must be a mapping");
    }
    ComputedOp op;
    for (const auto& [name, expr_node] : value.mapping()) {
      if (!expr_node.is_scalar()) {
        return base::ErrStatus(
            "pfgraph-yaml: computed column '%s' must be a string",
            name.c_str());
      }
      ColumnSpec col;
      col.expr = expr_node.scalar();
      col.alias = name;
      op.columns.push_back(std::move(col));
    }
    return Operation{std::move(op)};
  }

  // --- group_by ---
  if (key == "group_by") {
    if (!value.is_mapping()) {
      return base::ErrStatus("pfgraph-yaml: 'group_by' must be a mapping");
    }
    GroupByOp op;
    const YamlNode* columns = value.find("columns");
    if (columns) {
      std::vector<std::string> cols;
      ASSIGN_OR_RETURN(cols, GetStringList(*columns, "group_by.columns"));
      op.columns = std::move(cols);
    }
    const YamlNode* agg = value.find("agg");
    if (agg && agg->is_mapping()) {
      for (const auto& [name, expr_node] : agg->mapping()) {
        AggSpec spec;
        spec.result_name = name;
        if (!expr_node.is_scalar()) {
          return base::ErrStatus(
              "pfgraph-yaml: aggregation '%s' must be a string",
              name.c_str());
        }
        std::string expr = expr_node.scalar();
        // Try to parse structured "func(col)" pattern.
        // Match: identifier followed by ( content ) with nothing after.
        auto paren_open = expr.find('(');
        auto paren_close = expr.rfind(')');
        if (paren_open != std::string::npos &&
            paren_close == expr.size() - 1 && paren_close > paren_open) {
          std::string func_name = expr.substr(0, paren_open);
          std::string inner = expr.substr(paren_open + 1,
                                          paren_close - paren_open - 1);
          // Only use structured form if func_name is a simple identifier
          // (no spaces, operators, etc.) and inner has no nested parens.
          bool is_simple_func = !func_name.empty() &&
              func_name.find(' ') == std::string::npos &&
              func_name.find(',') == std::string::npos &&
              inner.find('(') == std::string::npos;
          if (is_simple_func) {
            spec.func = func_name;
            spec.column = inner;
          } else {
            spec.custom_expr = expr;
          }
        } else {
          spec.custom_expr = expr;
        }
        op.aggregations.push_back(std::move(spec));
      }
    }
    return Operation{std::move(op)};
  }

  // --- window ---
  if (key == "window") {
    if (!value.is_mapping()) {
      return base::ErrStatus("pfgraph-yaml: 'window' must be a mapping");
    }
    WindowOp op;
    for (const auto& [name, spec_node] : value.mapping()) {
      if (!spec_node.is_mapping()) {
        return base::ErrStatus(
            "pfgraph-yaml: window spec '%s' must be a mapping",
            name.c_str());
      }
      WindowSpec spec;
      spec.result_name = name;
      spec.func_expr = GetOptString(spec_node, "expr");
      const YamlNode* part = spec_node.find("partition");
      if (part) {
        std::vector<std::string> cols;
        ASSIGN_OR_RETURN(cols, GetStringList(*part, "window.partition"));
        spec.partition = std::move(cols);
      }
      spec.order_expr = GetOptString(spec_node, "order");
      spec.frame = GetOptString(spec_node, "frame");
      op.specs.push_back(std::move(spec));
    }
    return Operation{std::move(op)};
  }

  // --- sort ---
  if (key == "sort") {
    SortOp op;
    if (value.is_scalar()) {
      op.specs.push_back(ParseSortSpec(value.scalar()));
    } else if (value.is_sequence()) {
      for (const auto& item : value.sequence()) {
        if (!item.is_scalar()) {
          return base::ErrStatus(
              "pfgraph-yaml: 'sort' items must be strings");
        }
        op.specs.push_back(ParseSortSpec(item.scalar()));
      }
    } else {
      return base::ErrStatus(
          "pfgraph-yaml: 'sort' must be a string or list");
    }
    return Operation{std::move(op)};
  }

  // --- limit ---
  if (key == "limit") {
    if (!value.is_scalar()) {
      return base::ErrStatus("pfgraph-yaml: 'limit' must be a scalar");
    }
    LimitOp op;
    op.limit = value.as_int();
    return Operation{std::move(op)};
  }

  // --- offset ---
  if (key == "offset") {
    if (!value.is_scalar()) {
      return base::ErrStatus("pfgraph-yaml: 'offset' must be a scalar");
    }
    OffsetOp op;
    op.offset = value.as_int();
    return Operation{std::move(op)};
  }

  // --- distinct ---
  if (key == "distinct") {
    return Operation{DistinctOp{}};
  }

  // --- add_columns ---
  if (key == "add_columns") {
    if (!value.is_mapping()) {
      return base::ErrStatus(
          "pfgraph-yaml: 'add_columns' must be a mapping");
    }
    AddColumnsOp op;
    const YamlNode* from = value.find("from");
    if (from) {
      PipelineRef ref;
      ASSIGN_OR_RETURN(ref, ParsePipelineRefNode(*from, "add_columns.from"));
      op.from_ref = std::move(ref);
    }
    std::string on_str = GetOptString(value, "on");
    if (!on_str.empty()) {
      auto eq_pos = on_str.find(" = ");
      if (eq_pos != std::string::npos &&
          on_str.find(" AND ") == std::string::npos &&
          on_str.find(" OR ") == std::string::npos) {
        op.on_left_col = on_str.substr(0, eq_pos);
        op.on_right_col = on_str.substr(eq_pos + 3);
      } else {
        op.on_expr = on_str;
      }
    }
    const YamlNode* cols = value.find("cols");
    if (cols && cols->is_sequence()) {
      for (const auto& item : cols->sequence()) {
        if (!item.is_scalar()) {
          return base::ErrStatus(
              "pfgraph-yaml: 'add_columns.cols' items must be strings");
        }
        op.columns.push_back(ParseColumnSpec(item.scalar()));
      }
    }
    return Operation{std::move(op)};
  }

  // --- join (operation) ---
  if (key == "join") {
    if (!value.is_mapping()) {
      return base::ErrStatus("pfgraph-yaml: 'join' must be a mapping");
    }
    JoinOp op;
    const YamlNode* right = value.find("right");
    if (right) {
      PipelineRef ref;
      ASSIGN_OR_RETURN(ref, ParsePipelineRefNode(*right, "join.right"));
      op.right = std::move(ref);
    }
    op.on_expr = GetOptString(value, "on");
    std::string type_str = GetOptString(value, "type");
    if (!type_str.empty()) {
      op.is_left = base::CaseInsensitiveEqual(type_str, "LEFT");
    }
    return Operation{std::move(op)};
  }

  // --- cross_join ---
  if (key == "cross_join") {
    CrossJoinOp op;
    PipelineRef ref;
    ASSIGN_OR_RETURN(ref, ParsePipelineRefNode(value, "cross_join"));
    op.right = std::move(ref);
    return Operation{std::move(op)};
  }

  // --- span_join ---
  if (key == "span_join") {
    if (!value.is_mapping()) {
      return base::ErrStatus("pfgraph-yaml: 'span_join' must be a mapping");
    }
    SpanJoinOp op;
    const YamlNode* right = value.find("right");
    if (right) {
      PipelineRef ref;
      ASSIGN_OR_RETURN(ref, ParsePipelineRefNode(*right, "span_join.right"));
      op.right = std::move(ref);
    }
    const YamlNode* part = value.find("partition");
    if (part) {
      std::vector<std::string> cols;
      ASSIGN_OR_RETURN(cols, GetStringList(*part, "span_join.partition"));
      op.partition_columns = std::move(cols);
    }
    std::string type_str = GetOptString(value, "type");
    if (!type_str.empty()) {
      op.is_left = base::CaseInsensitiveEqual(type_str, "LEFT");
    }
    return Operation{std::move(op)};
  }

  // --- filter_during ---
  if (key == "filter_during") {
    if (!value.is_mapping()) {
      return base::ErrStatus(
          "pfgraph-yaml: 'filter_during' must be a mapping");
    }
    FilterDuringOp op;
    const YamlNode* intervals = value.find("intervals");
    if (intervals) {
      PipelineRef ref;
      ASSIGN_OR_RETURN(ref,
                       ParsePipelineRefNode(*intervals, "filter_during.intervals"));
      op.intervals = std::move(ref);
    }
    const YamlNode* part = value.find("partition");
    if (part) {
      std::vector<std::string> cols;
      ASSIGN_OR_RETURN(cols,
                       GetStringList(*part, "filter_during.partition"));
      op.partition_columns = std::move(cols);
    }
    std::string clip_str = GetOptString(value, "clip");
    if (!clip_str.empty()) {
      op.clip = (clip_str == "true");
    }
    return Operation{std::move(op)};
  }

  // --- filter_in ---
  if (key == "filter_in") {
    if (!value.is_mapping()) {
      return base::ErrStatus("pfgraph-yaml: 'filter_in' must be a mapping");
    }
    FilterInOp op;
    const YamlNode* match = value.find("match");
    if (match) {
      PipelineRef ref;
      ASSIGN_OR_RETURN(ref,
                       ParsePipelineRefNode(*match, "filter_in.match"));
      op.match_ref = std::move(ref);
    }
    op.base_column = GetOptString(value, "base_col");
    op.match_column = GetOptString(value, "match_col");
    return Operation{std::move(op)};
  }

  // --- except ---
  if (key == "except") {
    ExceptOp op;
    PipelineRef ref;
    ASSIGN_OR_RETURN(ref, ParsePipelineRefNode(value, "except"));
    op.other = std::move(ref);
    return Operation{std::move(op)};
  }

  // --- counter_to_intervals ---
  if (key == "counter_to_intervals") {
    return Operation{CounterToIntervalsOp{}};
  }

  // --- classify ---
  if (key == "classify") {
    if (!value.is_mapping()) {
      return base::ErrStatus("pfgraph-yaml: 'classify' must be a mapping");
    }
    ClassifyOp op;
    op.result_column = GetOptString(value, "column");
    op.source_column = GetOptString(value, "from");
    const YamlNode* rules = value.find("rules");
    if (rules && rules->is_mapping()) {
      for (const auto& [pattern, val_node] : rules->mapping()) {
        ClassifyMapping mapping;
        if (pattern == "_") {
          mapping.is_default = true;
        } else {
          mapping.pattern = pattern;
          mapping.is_default = false;
        }
        if (val_node.is_scalar()) {
          mapping.value = val_node.scalar();
        }
        op.mappings.push_back(std::move(mapping));
      }
    }
    return Operation{std::move(op)};
  }

  // --- extract_args ---
  if (key == "extract_args") {
    if (!value.is_mapping()) {
      return base::ErrStatus(
          "pfgraph-yaml: 'extract_args' must be a mapping");
    }
    ExtractArgsOp op;
    for (const auto& [name, path_node] : value.mapping()) {
      if (!path_node.is_scalar()) {
        return base::ErrStatus(
            "pfgraph-yaml: extract_args '%s' must be a string",
            name.c_str());
      }
      op.extractions.emplace_back(name, path_node.scalar());
    }
    return Operation{std::move(op)};
  }

  // --- find_ancestor ---
  if (key == "find_ancestor") {
    if (!value.is_mapping()) {
      return base::ErrStatus(
          "pfgraph-yaml: 'find_ancestor' must be a mapping");
    }
    FindAncestorOp op;
    op.where_expr = GetOptString(value, "where");
    const YamlNode* cols = value.find("cols");
    if (cols && cols->is_sequence()) {
      for (const auto& item : cols->sequence()) {
        if (!item.is_scalar()) {
          return base::ErrStatus(
              "pfgraph-yaml: 'find_ancestor.cols' items must be strings");
        }
        op.columns.push_back(ParseColumnSpec(item.scalar()));
      }
    }
    return Operation{std::move(op)};
  }

  // --- find_descendant ---
  if (key == "find_descendant") {
    if (!value.is_mapping()) {
      return base::ErrStatus(
          "pfgraph-yaml: 'find_descendant' must be a mapping");
    }
    FindDescendantOp op;
    op.where_expr = GetOptString(value, "where");
    const YamlNode* cols = value.find("cols");
    if (cols && cols->is_sequence()) {
      for (const auto& item : cols->sequence()) {
        if (!item.is_scalar()) {
          return base::ErrStatus(
              "pfgraph-yaml: 'find_descendant.cols' items must be strings");
        }
        op.columns.push_back(ParseColumnSpec(item.scalar()));
      }
    }
    return Operation{std::move(op)};
  }

  // --- flow_reachable ---
  if (key == "flow_reachable") {
    FlowReachableOp op;
    if (value.is_mapping()) {
      op.direction = GetOptString(value, "direction");
      if (op.direction.empty()) {
        op.direction = "out";
      }
    } else if (value.is_scalar()) {
      // flow_reachable: out
      op.direction = value.scalar();
    }
    return Operation{std::move(op)};
  }

  // --- flatten_intervals ---
  if (key == "flatten_intervals") {
    return Operation{FlattenIntervalsOp{}};
  }

  // --- merge_overlapping ---
  if (key == "merge_overlapping") {
    MergeOverlappingOp op;
    if (value.is_mapping()) {
      std::string eps = GetOptString(value, "epsilon");
      if (!eps.empty()) {
        op.epsilon = static_cast<int64_t>(std::stoll(eps));
      }
      const YamlNode* part = value.find("partition");
      if (part) {
        std::vector<std::string> cols;
        ASSIGN_OR_RETURN(cols,
                         GetStringList(*part, "merge_overlapping.partition"));
        op.partition_columns = std::move(cols);
      }
    }
    return Operation{std::move(op)};
  }

  // --- graph_reachable ---
  if (key == "graph_reachable") {
    if (!value.is_mapping()) {
      return base::ErrStatus(
          "pfgraph-yaml: 'graph_reachable' must be a mapping");
    }
    GraphReachableOp op;
    const YamlNode* edges = value.find("edges");
    if (edges) {
      PipelineRef ref;
      ASSIGN_OR_RETURN(ref,
                       ParsePipelineRefNode(*edges, "graph_reachable.edges"));
      op.edges = std::move(ref);
    }
    op.method = GetOptString(value, "method");
    if (op.method.empty()) {
      op.method = "dfs";
    }
    return Operation{std::move(op)};
  }

  // --- parse_name ---
  if (key == "parse_name") {
    if (!value.is_scalar()) {
      return base::ErrStatus("pfgraph-yaml: 'parse_name' must be a string");
    }
    ParseNameOp op;
    op.template_str = value.scalar();
    return Operation{std::move(op)};
  }

  // --- unpivot ---
  if (key == "unpivot") {
    if (!value.is_mapping()) {
      return base::ErrStatus("pfgraph-yaml: 'unpivot' must be a mapping");
    }
    UnpivotOp op;
    op.value_column = GetOptString(value, "value_col");
    if (op.value_column.empty())
      op.value_column = "value";
    op.name_column = GetOptString(value, "name_col");
    if (op.name_column.empty())
      op.name_column = "key";
    const YamlNode* cols = value.find("columns");
    if (cols) {
      std::vector<std::string> col_list;
      ASSIGN_OR_RETURN(col_list, GetStringList(*cols, "unpivot.columns"));
      op.source_columns = std::move(col_list);
    }
    return Operation{std::move(op)};
  }

  // --- pivot ---
  if (key == "pivot") {
    if (!value.is_mapping()) {
      return base::ErrStatus("pfgraph-yaml: 'pivot' must be a mapping");
    }
    PivotOp op;
    op.source_column = GetOptString(value, "from");
    op.value_column = GetOptString(value, "value");
    op.agg = GetOptString(value, "agg");
    if (op.agg.empty())
      op.agg = "max";
    const YamlNode* vals = value.find("values");
    if (vals && vals->is_mapping()) {
      for (const auto& [src_val, out_node] : vals->mapping()) {
        if (!out_node.is_scalar()) {
          return base::ErrStatus(
              "pfgraph-yaml: pivot value '%s' must be a string",
              src_val.c_str());
        }
        op.values.emplace_back(src_val, out_node.scalar());
      }
    }
    return Operation{std::move(op)};
  }

  // --- self_join_temporal ---
  if (key == "self_join_temporal") {
    if (!value.is_mapping()) {
      return base::ErrStatus(
          "pfgraph-yaml: 'self_join_temporal' must be a mapping");
    }
    SelfJoinTemporalOp op;
    op.left_key = GetOptString(value, "left_key");
    op.right_key = GetOptString(value, "right_key");
    op.overlap = GetOptString(value, "overlap");
    if (op.overlap.empty())
      op.overlap = "contains";
    op.right_alias = GetOptString(value, "alias");
    if (op.right_alias.empty())
      op.right_alias = "_other";
    std::string type_str = GetOptString(value, "type");
    if (!type_str.empty()) {
      op.is_left = base::CaseInsensitiveEqual(type_str, "LEFT");
    }
    return Operation{std::move(op)};
  }

  // --- index ---
  if (key == "index") {
    IndexOp op;
    if (value.is_scalar()) {
      op.columns.push_back(value.scalar());
    } else if (value.is_sequence()) {
      for (const auto& item : value.sequence()) {
        if (!item.is_scalar()) {
          return base::ErrStatus(
              "pfgraph-yaml: 'index' items must be strings");
        }
        op.columns.push_back(item.scalar());
      }
    }
    return Operation{std::move(op)};
  }

  return base::ErrStatus("pfgraph-yaml: unknown operation '%s'", key.c_str());
}

// ---------------------------------------------------------------------------
// Parse a pipeline: from + ops list.
// ---------------------------------------------------------------------------
base::StatusOr<Pipeline> ParsePipelineOps(const YamlNode& node) {
  if (!node.is_mapping()) {
    return base::ErrStatus(
        "pfgraph-yaml: pipeline must be a mapping with 'from' and 'ops'");
  }

  Pipeline pipeline;

  // Parse 'ops' key — a list where the first item is the source (from:),
  // and the rest are operations.
  const YamlNode* ops_node = node.find("ops");
  if (!ops_node || !ops_node->is_sequence() || ops_node->size() == 0) {
    return base::ErrStatus(
        "pfgraph-yaml: pipeline must have an 'ops' list with at least one item");
  }

  // First op must be the source (from:).
  const auto& first_op = ops_node->sequence()[0];
  if (!first_op.is_mapping()) {
    return base::ErrStatus(
        "pfgraph-yaml: first op must be a mapping with 'table' key");
  }
  const YamlNode* from_node = first_op.find("table");
  if (!from_node) {
    return base::ErrStatus(
        "pfgraph-yaml: first op in pipeline must be 'table:'");
  }
  Source src;
  ASSIGN_OR_RETURN(src, ParseSourceNode(*from_node));
  pipeline.source = std::move(src);

  // Remaining ops are operations.
  for (size_t i = 1; i < ops_node->size(); ++i) {
    const auto& op_node = ops_node->sequence()[i];
    if (!op_node.is_mapping()) {
      return base::ErrStatus(
          "pfgraph-yaml: each operation must be a mapping");
    }
    // Each op is a single-key mapping.
    for (const auto& [op_key, op_val] : op_node.mapping()) {
      Operation op;
      ASSIGN_OR_RETURN(op, ParseOperationEntry(op_key, op_val));
      pipeline.operations.push_back(std::move(op));
    }
  }

  return pipeline;
}

// ---------------------------------------------------------------------------
// Parse a FunctionDecl from a top-level YAML mapping entry.
// ---------------------------------------------------------------------------
base::StatusOr<FunctionDecl> ParseFunctionDecl(const std::string& name,
                                               const YamlNode& node) {
  FunctionDecl decl;
  decl.name = name;

  // Parse args.
  const YamlNode* args = node.find("args");
  if (args && args->is_mapping()) {
    for (const auto& [param_name, type_node] : args->mapping()) {
      FunctionParam param;
      param.name = param_name;
      if (type_node.is_scalar()) {
        param.type = type_node.scalar();
      }
      decl.params.push_back(std::move(param));
    }
  }

  // Parse returns.
  const YamlNode* returns = node.find("returns");
  if (returns && returns->is_scalar()) {
    std::string ret_str = returns->scalar();
    // Check for TABLE(...) syntax.
    if (base::StartsWith(ret_str, "TABLE(") &&
        base::EndsWith(ret_str, ")")) {
      std::string inner = ret_str.substr(6, ret_str.size() - 7);
      // Parse "col1: TYPE1, col2: TYPE2".
      for (const auto& part : base::SplitString(inner, ",")) {
        auto trimmed = base::StripSuffix(
            base::StripPrefix(part, " "), " ");
        auto colon_pos = trimmed.find(':');
        if (colon_pos != std::string::npos) {
          FunctionReturnCol col;
          col.name = std::string(
              base::StripSuffix(trimmed.substr(0, colon_pos), " "));
          auto type_sv = trimmed.substr(colon_pos + 1);
          col.type = std::string(base::StripPrefix(type_sv, " "));
          decl.return_cols.push_back(std::move(col));
        }
      }
    } else {
      decl.return_type = ret_str;
    }
  } else if (returns && returns->is_mapping()) {
    // TABLE return as mapping: {col: TYPE, ...}
    for (const auto& [col_name, type_node] : returns->mapping()) {
      FunctionReturnCol col;
      col.name = col_name;
      if (type_node.is_scalar()) {
        col.type = type_node.scalar();
      }
      decl.return_cols.push_back(std::move(col));
    }
  }

  // Parse body: either sql_body (string) or pipeline.
  const YamlNode* body = node.find("body");
  if (body) {
    if (body->is_scalar()) {
      decl.sql_body = body->scalar();
    } else if (body->is_mapping()) {
      // Pipeline body (has 'table' key).
      if (body->find("table")) {
        Pipeline pipeline;
        ASSIGN_OR_RETURN(pipeline, ParsePipelineOps(*body));
        decl.pipeline_body = std::move(pipeline);
      } else {
        decl.sql_body = GetOptString(*body, "sql");
      }
    }
  }

  return decl;
}

// ---------------------------------------------------------------------------
// Parse a NamedPipeline from a top-level YAML mapping entry.
// ---------------------------------------------------------------------------
base::StatusOr<NamedPipeline> ParseNamedPipeline(const std::string& name,
                                                 const YamlNode& node) {
  NamedPipeline np;
  np.name = name;

  // Parse annotation from 'type' key.
  std::string type_str = GetOptString(node, "type");
  if (type_str == "table") {
    np.annotation = PipelineAnnotation::kTable;
  } else if (type_str == "view") {
    np.annotation = PipelineAnnotation::kView;
  } else {
    np.annotation = PipelineAnnotation::kNone;
  }

  // Parse pipeline body.
  Pipeline pipeline;
  ASSIGN_OR_RETURN(pipeline, ParsePipelineOps(node));
  np.pipeline = std::move(pipeline);

  return np;
}

}  // namespace

// ============================================================================
// Public API.
// ============================================================================
base::StatusOr<GraphModule> ParsePfGraphYaml(std::string_view yaml_input) {
  YamlNode root;
  ASSIGN_OR_RETURN(root, ParseYaml(yaml_input));

  if (!root.is_mapping()) {
    return base::ErrStatus(
        "pfgraph-yaml: top-level YAML must be a mapping");
  }

  GraphModule mod;

  // Walk the top-level keys.
  for (const auto& [key, value] : root.mapping()) {
    // module: name
    if (key == "module") {
      if (!value.is_scalar()) {
        return base::ErrStatus(
            "pfgraph-yaml: 'module' must be a scalar string");
      }
      mod.module_name = value.scalar();
      continue;
    }

    // imports: [list]
    if (key == "imports") {
      std::vector<std::string> imports;
      ASSIGN_OR_RETURN(imports, GetStringList(value, "imports"));
      mod.imports = std::move(imports);
      continue;
    }

    // Everything else is a declaration (function or pipeline).
    if (!value.is_mapping()) {
      return base::ErrStatus(
          "pfgraph-yaml: declaration '%s' must be a mapping", key.c_str());
    }

    // Check if it's a function (has 'type: function').
    std::string type_str = GetOptString(value, "type");
    if (type_str == "function") {
      FunctionDecl func;
      ASSIGN_OR_RETURN(func, ParseFunctionDecl(key, value));
      mod.declarations.push_back(std::move(func));
    } else {
      NamedPipeline np;
      ASSIGN_OR_RETURN(np, ParseNamedPipeline(key, value));
      mod.declarations.push_back(std::move(np));
    }
  }

  return mod;
}

}  // namespace perfetto::trace_processor::pfgraph
