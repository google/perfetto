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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_TREE_FUNCTION_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_TREE_FUNCTION_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"

namespace perfetto::trace_processor::duckdb_integration {

// Pure-C++ reimplementation of PerfettoSQL's tree transform algorithms (the
// `_tree_from_table!` / `_tree_filter` / `_tree_propagate_down` /
// `_tree_to_table!` pipeline). The production SQLite path realises these via a
// bytecode interpreter (core/interpreter/bytecode_interpreter_impl.cc); the
// algorithms themselves are plain index arithmetic, lifted here so the DuckDB
// lane can run them natively (cf. the dominator_tree port).
namespace tree {

// A column's element type. Strings are owned std::string (the DuckDB lane has no
// StringPool dependency here); id/parent_id values and propagate outputs are
// Int64; durations etc. may be Double.
enum class ColType { kInt64, kDouble, kString };

// One column of the tree (uniform type, with a per-row null bitmap). column 0 is
// the original `id`, column 1 the original `parent_id`, the rest are passthrough
// (and propagate appends more).
struct Column {
  ColType type = ColType::kInt64;
  std::string name;
  std::vector<int64_t> i64;   // valid when type==kInt64
  std::vector<double> f64;    // valid when type==kDouble
  std::vector<std::string> str;  // valid when type==kString
  std::vector<bool> is_null;
};

// The tree: `parent[r]` is the row index of r's parent, or kNullParent for a
// root. `columns` are parallel (length row_count). Algorithms operate purely on
// these; the structural `parent` array drives traversal while the id/parent_id
// columns are just data echoed to the output.
struct Tree {
  static constexpr uint32_t kNullParent = 0xFFFFFFFF;
  uint32_t row_count = 0;
  std::vector<uint32_t> parent;
  std::vector<Column> columns;
};

enum class Op {
  kEq,
  kNe,
  kLt,
  kLe,
  kGt,
  kGe,
  kGlob,
  kIsNull,
  kIsNotNull,
};

// A filter constraint: column `name` `op` `value`. For kIsNull/kIsNotNull the
// value is ignored. The value carries its own type (matched against the column).
struct Constraint {
  std::string column;
  Op op;
  ColType value_type = ColType::kInt64;
  int64_t i64 = 0;
  double f64 = 0;
  std::string str;
};

enum class AggOp { kSum, kMin, kMax, kFirst, kLast };

struct PropagateSpec {
  AggOp op;
  std::string source_column;
  std::string output_column;
};

// Returns a new tree keeping only rows matching ALL constraints; a dropped
// node's children re-parent to the nearest surviving ancestor (or become roots).
// Unknown constraint columns are treated as "no rows match" only for that
// constraint via an error status.
base::StatusOr<Tree> FilterTree(const Tree& in,
                                const std::vector<Constraint>& constraints);

// Returns a new tree with, for each spec, an appended output column computed by
// a root->leaf BFS: out[child] = agg(out[parent], out[child]) (kFirst => whole
// subtree gets the root's value; kLast => each node keeps its own value).
base::StatusOr<Tree> PropagateDown(const Tree& in,
                                   const std::vector<PropagateSpec>& specs);

// Parses a propagate spec string `AGG(src) AS out`. Returns nullopt on a
// malformed spec or unknown AGG.
std::optional<PropagateSpec> ParsePropagateSpec(const std::string& spec);

}  // namespace tree

// Registers the DuckDB functions implementing the tree pipeline. (Bindings are
// added in a follow-up; the algorithm core above is the foundation.)
base::Status RegisterTreeFunctions(duckdb_connection conn);

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_TREE_FUNCTION_H_
