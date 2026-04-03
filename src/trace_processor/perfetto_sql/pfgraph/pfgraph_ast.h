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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PFGRAPH_PFGRAPH_AST_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PFGRAPH_PFGRAPH_AST_H_

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace perfetto::trace_processor::pfgraph {

// A reference to another named pipeline, optionally aliased.
struct PipelineRef {
  std::string name;
  std::string alias;  // Optional, for "ref AS alias".
};

// Column specification: name/expression with optional alias.
struct ColumnSpec {
  std::string expr;   // Column name or SQL expression.
  std::string alias;  // Optional alias (AS name).
};

// ============================================================================
// Source types (the FROM part of a pipeline).
// ============================================================================

// table('table_name')
struct TableSource {
  std::string table_name;
};

// slices(name: 'glob', thread: 'glob', process: 'glob', track: 'glob')
struct SlicesSource {
  std::string name_glob;
  std::string thread_glob;
  std::string process_glob;
  std::string track_glob;
};

// sql('SELECT ...')  or  sql('''multi-line SQL''')
struct SqlSource {
  std::string sql;
  std::vector<std::string> modules;  // Optional module dependencies.
};

// time_range(ts: 123, dur: 456) or time_range(dynamic: true)
struct TimeRangeSource {
  std::optional<int64_t> ts;
  std::optional<int64_t> dur;
  bool dynamic = false;
};

// interval_intersect(base, intervals..., partition: [cols])
struct IntervalIntersectSource {
  std::vector<PipelineRef> inputs;  // First is base, rest are intervals.
  std::vector<std::string> partition_columns;
};

// join(left, right, on: expr, type: INNER|LEFT)
struct JoinSource {
  PipelineRef left;
  PipelineRef right;
  std::string left_alias;   // Alias for left in condition.
  std::string right_alias;  // Alias for right in condition.
  std::string on_left_col;  // Equality join: left column.
  std::string on_right_col; // Equality join: right column.
  std::string on_expr;      // Freeform join condition (SQL).
  bool is_left_join = false; // false = INNER, true = LEFT.
};

// union(a, b, c, ...)
struct UnionSource {
  std::vector<PipelineRef> inputs;
  bool union_all = true;
};

// create_slices(starts: ref, ends: ref, starts_ts: col, ends_ts: col)
struct CreateSlicesSource {
  PipelineRef starts;
  PipelineRef ends;
  std::string starts_ts_col;  // Default: "ts".
  std::string ends_ts_col;    // Default: "ts".
};

// lookup_table('key1' => val1, 'key2' => val2, ...)
struct LookupTableSource {
  std::vector<std::pair<std::string, std::string>> entries;  // key, value pairs
};

// Template call as source: template_name(arg1, arg2, ...)
struct TemplateCallSource {
  std::string template_name;
  std::vector<std::pair<std::string, std::string>> args;
};

using Source = std::variant<TableSource,
                            SlicesSource,
                            SqlSource,
                            TimeRangeSource,
                            IntervalIntersectSource,
                            JoinSource,
                            UnionSource,
                            CreateSlicesSource,
                            LookupTableSource,
                            TemplateCallSource,
                            PipelineRef>;

// ============================================================================
// Operation types (chained after source with .op()).
// ============================================================================

// .filter(SQL_EXPR)
struct FilterOp {
  std::string expr;  // SQL WHERE clause expression.
};

// .select(col1, col2 AS alias, ...)
struct SelectOp {
  std::vector<ColumnSpec> columns;
};

// .add_columns(from: ref, on: left_col = right_col, cols: [col, ...])
struct AddColumnsOp {
  PipelineRef from_ref;
  std::string on_left_col;
  std::string on_right_col;
  std::string on_expr;  // Freeform condition (alternative to columns).
  std::vector<ColumnSpec> columns;
};

// Aggregation specification: name: func(col) or name: func(col, percentile)
struct AggSpec {
  std::string result_name;  // Output column name.
  std::string func;         // sum, count, min, max, mean, median, etc.
  std::string column;       // Column to aggregate (empty for count()).
  double percentile = 0;    // For percentile() function.
  std::string custom_expr;  // For custom SQL aggregation expressions.
};

// .group_by(col1, col2).agg(name: sum(col), ...)
struct GroupByOp {
  std::vector<std::string> columns;
  std::vector<AggSpec> aggregations;
};

// .sort(col1 DESC, col2 ASC)
struct SortSpec {
  std::string column;
  bool desc = false;
};

struct SortOp {
  std::vector<SortSpec> specs;
};

// .limit(N)
struct LimitOp {
  int64_t limit;
};

// .offset(N)
struct OffsetOp {
  int64_t offset;
};

// .counter_to_intervals()
struct CounterToIntervalsOp {};

// .filter_during(interval_ref, partition: [...], clip: true/false)
struct FilterDuringOp {
  PipelineRef intervals;
  std::vector<std::string> partition_columns;
  bool clip = true;
};

// .filter_in(match_ref, base_col: col, match_col: col)
struct FilterInOp {
  PipelineRef match_ref;
  std::string base_column;
  std::string match_column;
};

// ============================================================================
// NEW: Higher-level operations
// ============================================================================

// .window(name: func(col) over (partition: [cols], order: col), ...)
// Adds window function columns to the result.
struct WindowSpec {
  std::string result_name;             // Output column name.
  std::string func_expr;               // e.g., "lag(state)", "lead(ts, 1, trace_end())"
  std::vector<std::string> partition;  // PARTITION BY columns.
  std::string order_expr;              // ORDER BY expression (may be multi-column).
  std::string frame;                   // Optional frame spec (raw SQL).
};

struct WindowOp {
  std::vector<WindowSpec> specs;
};

// .computed(name: expr, ...)
// Adds derived columns (SELECT *, expr1 AS name1, expr2 AS name2).
struct ComputedOp {
  std::vector<ColumnSpec> columns;  // Each is expr AS alias.
};

// .classify(result_col, from: source_col, 'pattern' => 'value', ...)
// CASE/WHEN classification based on GLOB patterns or exact match.
struct ClassifyMapping {
  std::string pattern;   // GLOB pattern, exact value, or "_" for default.
  std::string value;     // The mapped value.
  bool is_default = false;
};

struct ClassifyOp {
  std::string result_column;
  std::string source_column;
  std::vector<ClassifyMapping> mappings;
};

// .extract_args(name: 'arg.path', ...)
// Bulk extraction from arg_set_id.
struct ExtractArgsOp {
  // Each pair is (result_column_name, arg_path).
  std::vector<std::pair<std::string, std::string>> extractions;
};

// .distinct()
struct DistinctOp {};

// .except(other_ref)
struct ExceptOp {
  PipelineRef other;
};

// .span_join(right_ref, partition: [...], type: LEFT|INNER)
// SPAN_JOIN or SPAN_LEFT_JOIN virtual table pattern.
struct SpanJoinOp {
  PipelineRef right;
  std::vector<std::string> partition_columns;
  bool is_left = false;
};

// .unpivot(value_col: name, name_col: name, columns: [col1, col2, ...])
// Transforms columns into rows via UNION ALL.
struct UnpivotOp {
  std::string value_column;
  std::string name_column;
  std::vector<std::string> source_columns;
};

// .index(col1, col2, ...)
// CREATE PERFETTO INDEX on this table.
struct IndexOp {
  std::vector<std::string> columns;
};

// .find_ancestor(where: expr, cols: [col AS alias, ...])
// For each row, find the nearest ancestor slice matching a condition.
// Compiles to LEFT JOIN ancestor_slice(base.id) AS _anc ON expr.
struct FindAncestorOp {
  std::string where_expr;            // Filter condition on the ancestor.
  std::vector<ColumnSpec> columns;   // Columns to bring from the ancestor.
};

// .find_descendant(where: expr, cols: [col AS alias, ...])
// For each row, find a descendant slice matching a condition.
struct FindDescendantOp {
  std::string where_expr;
  std::vector<ColumnSpec> columns;
};

// .parse_name('template{field1}sep{field2}...')
// Desugars to .computed() with str_split/substr calls.
struct ParseNameOp {
  std::string template_str;  // e.g., "ErrorId:{process_name} {pid}#{error_id}"
};

// .closest_preceding(other, match: col = col, order: ts)
// Temporal as-of join: for each row, find the most recent matching row in other.
struct ClosestPrecedingOp {
  PipelineRef other;
  std::string match_left_col;
  std::string match_right_col;
  std::string order_expr;
};

// .join(right_ref, on: expr, type: LEFT|INNER)
// Chainable join: current pipeline is the left side.
struct JoinOp {
  PipelineRef right;
  std::string on_expr;  // Freeform join condition.
  bool is_left = false;
};

// .cross_join(right_ref)
// Cross join with another table.
struct CrossJoinOp {
  PipelineRef right;
};

// .flow_reachable(direction: 'out')
// Follow flow edges from slice IDs.
struct FlowReachableOp {
  std::string direction = "out";  // "out" or "in".
};

// .flatten_intervals()
// Flatten interval hierarchy to leaf intervals.
struct FlattenIntervalsOp {};

// .merge_overlapping(epsilon: 0, partition: [cols])
// Merge overlapping intervals.
struct MergeOverlappingOp {
  int64_t epsilon = 0;
  std::vector<std::string> partition_columns;
};

// .graph_reachable(edges_ref, method: 'dfs')
// DFS/BFS graph reachability from current nodes.
struct GraphReachableOp {
  PipelineRef edges;
  std::string method = "dfs";  // "dfs" or "bfs".
};

// .pivot(from: col, value: col, agg: func, values: {src_val: out_col, ...})
// Transforms rows into wide columns using explicit value mapping.
// Compiles to GROUP BY + CASE WHEN for each value.
struct PivotOp {
  std::string source_column;  // Column whose values are pivoted.
  std::string value_column;   // Column providing the values to aggregate.
  std::string agg;            // Aggregation function (default: "max").
  // Explicit mapping: source value => output column name.
  std::vector<std::pair<std::string, std::string>> values;
};

// .self_join_temporal(left_key, right_key, overlap, prefix)
// Self-join a table on key match + time overlap.
// Materializes the inner query, then joins it to itself.
struct SelfJoinTemporalOp {
  std::string left_key;        // Column on left (base) side to match.
  std::string right_key;       // Column on right side to match.
  std::string overlap;         // "contains" (default) or "intersects".
  std::string right_alias;     // Alias for right side (default: "_other").
  bool is_left = false;        // LEFT JOIN vs INNER JOIN.
};

// Template call as an operation: .template_name(arg1, arg2, ...)
// Expanded at compile time using the template definition.
struct TemplateCallOp {
  std::string template_name;
  std::vector<std::pair<std::string, std::string>> args;  // name: value pairs
};

using Operation = std::variant<FilterOp,
                               SelectOp,
                               AddColumnsOp,
                               GroupByOp,
                               SortOp,
                               LimitOp,
                               OffsetOp,
                               CounterToIntervalsOp,
                               FilterDuringOp,
                               FilterInOp,
                               WindowOp,
                               ComputedOp,
                               ClassifyOp,
                               ExtractArgsOp,
                               DistinctOp,
                               ExceptOp,
                               SpanJoinOp,
                               UnpivotOp,
                               IndexOp,
                               FindAncestorOp,
                               FindDescendantOp,
                               ParseNameOp,
                               ClosestPrecedingOp,
                               TemplateCallOp,
                               JoinOp,
                               CrossJoinOp,
                               FlowReachableOp,
                               FlattenIntervalsOp,
                               MergeOverlappingOp,
                               GraphReachableOp,
                               PivotOp,
                               SelfJoinTemporalOp>;

// ============================================================================
// Pipeline and top-level structures.
// ============================================================================

// A pipeline is a source followed by zero or more chained operations.
struct Pipeline {
  Source source;
  std::vector<Operation> operations;
};

// Annotation for a named pipeline.
enum class PipelineAnnotation {
  kNone,   // Intermediate (becomes CTE or temp table).
  kTable,  // CREATE PERFETTO TABLE.
  kView,   // CREATE PERFETTO VIEW.
};

// A named pipeline declaration.
struct NamedPipeline {
  std::string name;
  PipelineAnnotation annotation = PipelineAnnotation::kNone;
  Pipeline pipeline;
  std::vector<std::string> index_columns;  // Optional @index annotation.
  uint32_t line = 0;
};

// A raw SQL block (@sql { ... }).
struct SqlBlock {
  std::string sql;
  uint32_t line = 0;
};

// A function parameter.
struct FunctionParam {
  std::string name;
  std::string type;  // INT, STRING, LONG, etc.
};

// A function return column (for table-returning functions).
struct FunctionReturnCol {
  std::string name;
  std::string type;
};

// @function declaration: either scalar or table-returning.
struct FunctionDecl {
  std::string name;
  std::vector<FunctionParam> params;
  // For scalar functions: return_type is set, return_cols is empty.
  std::string return_type;
  // For table-returning functions: return_cols is set, return_type is empty.
  std::vector<FunctionReturnCol> return_cols;
  // Body is either a Pipeline or raw SQL expression.
  std::optional<Pipeline> pipeline_body;
  std::string sql_body;  // For scalar functions or complex bodies.
  uint32_t line = 0;
};

// @define template declaration.
struct TemplateParam {
  std::string name;
  std::string type;  // Column, String, Int, Pipeline
};

struct TemplateDecl {
  std::string name;
  std::vector<TemplateParam> params;
  bool is_operation;  // true = body starts with '.', false = body starts with source
  Pipeline body;      // The template body (with $param references).
  uint32_t line = 0;
};

// Top-level declaration: named pipeline, SQL block, function, or template.
using Declaration = std::variant<NamedPipeline, SqlBlock, FunctionDecl, TemplateDecl>;

// The top-level AST node representing an entire .pfgraph file.
struct GraphModule {
  std::string module_name;           // From `module` declaration.
  std::vector<std::string> imports;  // From `import` declarations.
  std::vector<Declaration> declarations;
};

}  // namespace perfetto::trace_processor::pfgraph

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PFGRAPH_PFGRAPH_AST_H_
