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

#include "src/trace_processor/duckdb/tree_function.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "duckdb.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/duckdb/udf_handle_registry.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace tree {
namespace {

// Returns the row indices in root->leaf topological order (parents before
// children), via a BFS over the children derived from `parent`.
std::vector<uint32_t> BfsOrder(const std::vector<uint32_t>& parent) {
  uint32_t n = static_cast<uint32_t>(parent.size());
  std::vector<std::vector<uint32_t>> children(n);
  std::vector<uint32_t> roots;
  for (uint32_t r = 0; r < n; ++r) {
    if (parent[r] == Tree::kNullParent) {
      roots.push_back(r);
    } else {
      children[parent[r]].push_back(r);
    }
  }
  std::vector<uint32_t> order;
  order.reserve(n);
  std::deque<uint32_t> q(roots.begin(), roots.end());
  while (!q.empty()) {
    uint32_t v = q.front();
    q.pop_front();
    order.push_back(v);
    for (uint32_t c : children[v]) {
      q.push_back(c);
    }
  }
  return order;
}

int FindColumn(const Tree& t, const std::string& name) {
  for (size_t i = 0; i < t.columns.size(); ++i) {
    if (t.columns[i].name == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// Ordered comparison via operator< only (so the float case avoids -Wfloat-equal
// and the same code serves int64/double/string). All Op values are listed.
template <typename T>
bool CmpOp(Op op, const T& a, const T& b) {
  switch (op) {
    case Op::kEq:
      return !(a < b) && !(b < a);
    case Op::kNe:
      return (a < b) || (b < a);
    case Op::kLt:
      return a < b;
    case Op::kLe:
      return !(b < a);
    case Op::kGt:
      return b < a;
    case Op::kGe:
      return !(a < b);
    case Op::kGlob:
    case Op::kIsNull:
    case Op::kIsNotNull:
      return false;
  }
  return false;
}

// Evaluates whether row `r` of `col` matches `c`. NULLs never satisfy a
// comparison op (SQL semantics); IS [NOT] NULL test the null bitmap.
bool MatchRow(const Column& col, uint32_t r, const Constraint& c) {
  bool is_null = col.is_null[r];
  if (c.op == Op::kIsNull) {
    return is_null;
  }
  if (c.op == Op::kIsNotNull) {
    return !is_null;
  }
  if (is_null) {
    return false;
  }
  // Numeric comparison (int/double cross-compared as double); string only with
  // string. A type mismatch (e.g. number vs string) never matches.
  bool col_num = col.type != ColType::kString;
  bool val_num = c.value_type != ColType::kString;
  if (col_num && val_num) {
    double lhs = col.type == ColType::kInt64 ? static_cast<double>(col.i64[r])
                                             : col.f64[r];
    double rhs =
        c.value_type == ColType::kInt64 ? static_cast<double>(c.i64) : c.f64;
    return CmpOp<double>(c.op, lhs, rhs);
  }
  if (!col_num && !val_num) {
    return CmpOp<std::string>(c.op, col.str[r], c.str);
  }
  return false;
}

// Appends row `src_row` of `src` to `dst` (same type).
void AppendRow(Column& dst, const Column& src, uint32_t src_row) {
  dst.is_null.push_back(src.is_null[src_row]);
  switch (src.type) {
    case ColType::kInt64:
      dst.i64.push_back(src.i64[src_row]);
      break;
    case ColType::kDouble:
      dst.f64.push_back(src.f64[src_row]);
      break;
    case ColType::kString:
      dst.str.push_back(src.str[src_row]);
      break;
  }
}

}  // namespace

base::StatusOr<Tree> FilterTree(const Tree& in,
                                const std::vector<Constraint>& constraints) {
  // Resolve constraint columns up-front.
  std::vector<int> cols;
  cols.reserve(constraints.size());
  for (const Constraint& c : constraints) {
    int idx = FindColumn(in, c.column);
    if (idx < 0) {
      return base::ErrStatus("tree filter: unknown column '%s'",
                             c.column.c_str());
    }
    if (c.op == Op::kGlob) {
      return base::ErrStatus("tree filter: GLOB not supported in DuckDB lane");
    }
    cols.push_back(idx);
  }

  uint32_t n = in.row_count;
  std::vector<bool> keep(n, true);
  for (uint32_t r = 0; r < n; ++r) {
    for (size_t k = 0; k < constraints.size(); ++k) {
      if (!MatchRow(in.columns[static_cast<size_t>(cols[k])], r,
                    constraints[k])) {
        keep[r] = false;
        break;
      }
    }
  }

  // Nearest surviving ancestor (inclusive), computed root->leaf.
  std::vector<uint32_t> sa(n, Tree::kNullParent);
  for (uint32_t v : BfsOrder(in.parent)) {
    if (keep[v]) {
      sa[v] = v;
    } else {
      sa[v] = in.parent[v] == Tree::kNullParent ? Tree::kNullParent
                                                : sa[in.parent[v]];
    }
  }

  // Dense new indices for kept rows, in original order.
  std::vector<uint32_t> old_to_new(n, Tree::kNullParent);
  uint32_t kept = 0;
  for (uint32_t r = 0; r < n; ++r) {
    if (keep[r]) {
      old_to_new[r] = kept++;
    }
  }

  Tree out;
  out.row_count = kept;
  out.parent.resize(kept);
  out.columns.resize(in.columns.size());
  for (size_t c = 0; c < in.columns.size(); ++c) {
    out.columns[c].type = in.columns[c].type;
    out.columns[c].name = in.columns[c].name;
  }
  for (uint32_t r = 0; r < n; ++r) {
    if (!keep[r]) {
      continue;
    }
    uint32_t ni = old_to_new[r];
    uint32_t op = in.parent[r];
    uint32_t anc = op == Tree::kNullParent ? Tree::kNullParent : sa[op];
    out.parent[ni] =
        anc == Tree::kNullParent ? Tree::kNullParent : old_to_new[anc];
    for (size_t c = 0; c < in.columns.size(); ++c) {
      AppendRow(out.columns[c], in.columns[c], r);
    }
  }
  return out;
}

base::StatusOr<Tree> PropagateDown(const Tree& in,
                                   const std::vector<PropagateSpec>& specs) {
  Tree out = in;  // structure + existing columns unchanged.
  std::vector<uint32_t> order = BfsOrder(in.parent);
  for (const PropagateSpec& spec : specs) {
    int src = FindColumn(out, spec.source_column);
    if (src < 0) {
      return base::ErrStatus("tree propagate: unknown column '%s'",
                             spec.source_column.c_str());
    }
    const Column& src_col = out.columns[static_cast<size_t>(src)];
    if (src_col.type == ColType::kString) {
      return base::ErrStatus("tree propagate: cannot aggregate string column");
    }
    // New column seeded from source.
    Column dst = src_col;
    dst.name = spec.output_column;
    bool is_int = dst.type == ColType::kInt64;
    for (uint32_t v : order) {
      uint32_t p = out.parent[v];
      if (p == Tree::kNullParent) {
        continue;
      }
      if (spec.op == AggOp::kLast) {
        continue;  // each node keeps its own value.
      }
      if (spec.op == AggOp::kFirst) {
        if (is_int) {
          dst.i64[v] = dst.i64[p];
        } else {
          dst.f64[v] = dst.f64[p];
        }
        dst.is_null[v] = dst.is_null[p];
        continue;
      }
      if (is_int) {
        int64_t a = dst.i64[p], b = dst.i64[v];
        dst.i64[v] = spec.op == AggOp::kSum   ? a + b
                     : spec.op == AggOp::kMin ? std::min(a, b)
                                              : std::max(a, b);
      } else {
        double a = dst.f64[p], b = dst.f64[v];
        dst.f64[v] = spec.op == AggOp::kSum   ? a + b
                     : spec.op == AggOp::kMin ? std::min(a, b)
                                              : std::max(a, b);
      }
    }
    out.columns.push_back(std::move(dst));
  }
  return out;
}

std::optional<PropagateSpec> ParsePropagateSpec(const std::string& spec) {
  size_t lp = spec.find('(');
  size_t rp = spec.find(')', lp == std::string::npos ? 0 : lp);
  if (lp == std::string::npos || rp == std::string::npos || rp < lp) {
    return std::nullopt;
  }
  std::string agg = base::ToLower(base::TrimWhitespace(spec.substr(0, lp)));
  std::string src = base::TrimWhitespace(spec.substr(lp + 1, rp - lp - 1));
  // After ')', expect (case-insensitive) ` AS <name>`.
  std::string rest = base::TrimWhitespace(spec.substr(rp + 1));
  std::string rest_low = base::ToLower(rest);
  if (rest_low.rfind("as", 0) != 0 || rest.size() < 3 ||
      (rest[2] != ' ' && rest[2] != '\t')) {
    return std::nullopt;
  }
  std::string out = base::TrimWhitespace(rest.substr(2));
  if (src.empty() || out.empty()) {
    return std::nullopt;
  }
  PropagateSpec ps;
  if (agg == "sum") {
    ps.op = AggOp::kSum;
  } else if (agg == "min") {
    ps.op = AggOp::kMin;
  } else if (agg == "max") {
    ps.op = AggOp::kMax;
  } else if (agg == "first") {
    ps.op = AggOp::kFirst;
  } else if (agg == "last") {
    ps.op = AggOp::kLast;
  } else {
    return std::nullopt;
  }
  ps.source_column = src;
  ps.output_column = out;
  return ps;
}

}  // namespace tree

// ===========================================================================
// DuckDB bindings.
//
// The tree pipeline is realised with the same aggregate->handle->combiner
// mechanism as interval_intersect/dominator (table functions can't take
// non-constant args in DuckDB, scalars/aggregates can):
//   * `__intrinsic_tree_from_table(id, parent_id, passthrough_struct)` is an
//     AGGREGATE that collects every source row into a tree::Tree and returns an
//     opaque BIGINT handle (HandleRegistry<Tree>). Aggregates can't be variadic
//     in the C API, so the (variable) passthrough columns are packed into a
//     single ANY-typed STRUCT by the `_tree_from_table` macro override.
//   * `__intrinsic_tree_constraint`/`_tree_where_and`/`_tree_filter`/
//     `_tree_propagate_down` are SCALARS taking/returning handles, applying the
//     pure-C++ tree::* algorithms.
//   * `__intrinsic_tree_to_table(handle, name...)` is a SCALAR returning a
//     LIST<STRUCT> of result rows (UNNESTed by the `_tree_to_table` override).
//     Passthrough columns are type-erased into a UNION(i,d,s) per column so the
//     return type is static; the DuckDb iterator decodes the active member back
//     to its natural SqlValue type per row (cf. extract_arg).
// ===========================================================================
namespace {

using tree::ColType;
using tree::Column;
using tree::Constraint;
using tree::Tree;

// The UNION member ordinals for type-erased passthrough columns, matching the
// physical layout (struct child 0 = UTINYINT tag, child member+1 = member vec).
enum UnionMember : uint8_t { kIntM = 0, kDoubleM = 1, kStringM = 2 };

std::string_view ReadVarchar(duckdb_vector vec, idx_t row) {
  auto* data = static_cast<duckdb_string_t*>(duckdb_vector_get_data(vec));
  duckdb_string_t* s = &data[row];
  return std::string_view(duckdb_string_t_data(s), duckdb_string_t_length(*s));
}

bool IsRowNull(duckdb_vector vec, idx_t row) {
  uint64_t* validity = duckdb_vector_get_validity(vec);
  return validity && !duckdb_validity_row_is_valid(validity, row);
}

ColType MapColType(duckdb_type tid) {
  switch (static_cast<int>(tid)) {
    case DUCKDB_TYPE_FLOAT:
    case DUCKDB_TYPE_DOUBLE:
    case DUCKDB_TYPE_DECIMAL:
      return ColType::kDouble;
    case DUCKDB_TYPE_VARCHAR:
      return ColType::kString;
    default:
      return ColType::kInt64;  // all integral widths widen to int64.
  }
}

int64_t ReadInt64(duckdb_vector vec, duckdb_type tid, idx_t row) {
  void* d = duckdb_vector_get_data(vec);
  switch (static_cast<int>(tid)) {
    case DUCKDB_TYPE_BOOLEAN:
      return static_cast<bool*>(d)[row] ? 1 : 0;
    case DUCKDB_TYPE_TINYINT:
      return static_cast<int8_t*>(d)[row];
    case DUCKDB_TYPE_SMALLINT:
      return static_cast<int16_t*>(d)[row];
    case DUCKDB_TYPE_INTEGER:
      return static_cast<int32_t*>(d)[row];
    case DUCKDB_TYPE_BIGINT:
      return static_cast<int64_t*>(d)[row];
    case DUCKDB_TYPE_UTINYINT:
      return static_cast<uint8_t*>(d)[row];
    case DUCKDB_TYPE_USMALLINT:
      return static_cast<uint16_t*>(d)[row];
    case DUCKDB_TYPE_UINTEGER:
      return static_cast<uint32_t*>(d)[row];
    case DUCKDB_TYPE_UBIGINT:
      return static_cast<int64_t>(static_cast<uint64_t*>(d)[row]);
    default:
      return 0;
  }
}

double ReadDouble(duckdb_vector vec, duckdb_type tid, idx_t row) {
  void* d = duckdb_vector_get_data(vec);
  if (tid == DUCKDB_TYPE_FLOAT) {
    return static_cast<double>(static_cast<float*>(d)[row]);
  }
  return static_cast<double*>(d)[row];
}

// Appends value at `row` of vector `vec` (of duckdb type `tid`) to `col` (whose
// tree::ColType was derived from `tid` via MapColType).
void AppendCell(Column& col, duckdb_vector vec, duckdb_type tid, idx_t row) {
  if (IsRowNull(vec, row)) {
    col.is_null.push_back(true);
    switch (col.type) {
      case ColType::kInt64:
        col.i64.push_back(0);
        break;
      case ColType::kDouble:
        col.f64.push_back(0);
        break;
      case ColType::kString:
        col.str.emplace_back();
        break;
    }
    return;
  }
  col.is_null.push_back(false);
  switch (col.type) {
    case ColType::kInt64:
      col.i64.push_back(ReadInt64(vec, tid, row));
      break;
    case ColType::kDouble:
      col.f64.push_back(ReadDouble(vec, tid, row));
      break;
    case ColType::kString:
      col.str.emplace_back(ReadVarchar(vec, row));
      break;
  }
}

// ---------------------------------------------------------------------------
// __intrinsic_tree_from_table aggregate.
// ---------------------------------------------------------------------------

// Collected rows (scan order) before the structural parent[] is resolved at
// Finalize. `cols` is [id, parent_id, passthrough...]; the passthrough schema
// is captured lazily from the first chunk's STRUCT vector.
struct FromBuffer {
  bool schema_init = false;
  std::vector<duckdb_type> pt_types;  // passthrough child duckdb types.
  std::vector<Column> cols;           // cols[0]=id, [1]=parent_id, [2+]=pt.
  std::vector<int64_t> parent_id_val;
  std::vector<bool> parent_id_null;
};

using FromState = FromBuffer*;

idx_t FromStateSize(duckdb_function_info) {
  return sizeof(FromState);
}
void FromInit(duckdb_function_info, duckdb_aggregate_state state) {
  *reinterpret_cast<FromState*>(state) = nullptr;
}

void FromUpdate(duckdb_function_info,
                duckdb_data_chunk input,
                duckdb_aggregate_state* states) {
  idx_t rows = duckdb_data_chunk_get_size(input);
  duckdb_vector id_vec = duckdb_data_chunk_get_vector(input, 0);
  duckdb_vector pid_vec = duckdb_data_chunk_get_vector(input, 1);
  duckdb_vector names_vec = duckdb_data_chunk_get_vector(input, 2);
  duckdb_vector pt_vec = duckdb_data_chunk_get_vector(input, 3);

  // The passthrough columns arrive as a parallel pair: a VARCHAR[] of names and
  // a positional STRUCT of values (DuckDB named-struct syntax can't pass
  // through the SQLite-grammar macro tokenizer). Introspect the struct's child
  // types and read the names from the (constant) list. Same across rows; cached
  // on first sight.
  duckdb_logical_type pt_type = duckdb_vector_get_column_type(pt_vec);
  idx_t pt_n = duckdb_struct_type_child_count(pt_type);
  std::vector<duckdb_vector> pt_children(pt_n);
  std::vector<duckdb_type> pt_child_types(pt_n);
  for (idx_t c = 0; c < pt_n; ++c) {
    pt_children[c] = duckdb_struct_vector_get_child(pt_vec, c);
    duckdb_logical_type ct = duckdb_struct_type_child_type(pt_type, c);
    pt_child_types[c] = duckdb_get_type_id(ct);
    duckdb_destroy_logical_type(&ct);
  }
  duckdb_destroy_logical_type(&pt_type);
  // Names from the list vector (constant per call; read element 0..pt_n-1 of
  // the first row's list).
  std::vector<std::string> pt_names(pt_n);
  if (rows > 0) {
    auto* list_entries =
        static_cast<duckdb_list_entry*>(duckdb_vector_get_data(names_vec));
    duckdb_vector names_child = duckdb_list_vector_get_child(names_vec);
    idx_t base = list_entries[0].offset;
    for (idx_t c = 0; c < pt_n && c < list_entries[0].length; ++c) {
      pt_names[c] = std::string(ReadVarchar(names_child, base + c));
    }
  }

  for (idx_t row = 0; row < rows; ++row) {
    FromState& slot = *reinterpret_cast<FromState*>(states[row]);
    if (!slot) {
      slot = new FromBuffer();
    }
    FromBuffer& buf = *slot;
    if (!buf.schema_init) {
      buf.schema_init = true;
      buf.cols.resize(2 + pt_n);
      buf.cols[0].type = ColType::kInt64;
      buf.cols[0].name = "id";
      buf.cols[1].type = ColType::kInt64;
      buf.cols[1].name = "parent_id";
      buf.pt_types = pt_child_types;
      for (idx_t c = 0; c < pt_n; ++c) {
        buf.cols[2 + c].type = MapColType(pt_child_types[c]);
        buf.cols[2 + c].name = pt_names[c];
      }
    }
    // id (column 0); rows with a NULL id cannot be addressed as a parent, but
    // are still kept as nodes (id stored as 0/null).
    AppendCell(buf.cols[0], id_vec, DUCKDB_TYPE_BIGINT, row);
    // parent_id retained both as a data column (col 1) and as the raw value
    // used to resolve parent[] at Finalize.
    AppendCell(buf.cols[1], pid_vec, DUCKDB_TYPE_BIGINT, row);
    if (IsRowNull(pid_vec, row)) {
      buf.parent_id_null.push_back(true);
      buf.parent_id_val.push_back(0);
    } else {
      buf.parent_id_null.push_back(false);
      buf.parent_id_val.push_back(ReadInt64(pid_vec, DUCKDB_TYPE_BIGINT, row));
    }
    for (idx_t c = 0; c < pt_n; ++c) {
      AppendCell(buf.cols[2 + c], pt_children[c], buf.pt_types[c], row);
    }
  }
}

void FromCombine(duckdb_function_info,
                 duckdb_aggregate_state* source,
                 duckdb_aggregate_state* target,
                 idx_t count) {
  for (idx_t i = 0; i < count; ++i) {
    FromState src = *reinterpret_cast<FromState*>(source[i]);
    if (!src) {
      continue;
    }
    FromState& dst = *reinterpret_cast<FromState*>(target[i]);
    if (!dst) {
      // Steal the source buffer wholesale: cheaper than appending and there is
      // no ordering guarantee to preserve across partial-aggregate merges.
      dst = new FromBuffer(std::move(*src));
      continue;
    }
    FromBuffer& d = *dst;
    FromBuffer& s = *src;
    if (!d.schema_init) {
      d = std::move(s);
      continue;
    }
    for (size_t c = 0; c < s.cols.size() && c < d.cols.size(); ++c) {
      const Column& sc = s.cols[c];
      Column& dc = d.cols[c];
      dc.is_null.insert(dc.is_null.end(), sc.is_null.begin(), sc.is_null.end());
      dc.i64.insert(dc.i64.end(), sc.i64.begin(), sc.i64.end());
      dc.f64.insert(dc.f64.end(), sc.f64.begin(), sc.f64.end());
      dc.str.insert(dc.str.end(), sc.str.begin(), sc.str.end());
    }
    d.parent_id_val.insert(d.parent_id_val.end(), s.parent_id_val.begin(),
                           s.parent_id_val.end());
    d.parent_id_null.insert(d.parent_id_null.end(), s.parent_id_null.begin(),
                            s.parent_id_null.end());
  }
}

// Resolves the structural parent[] from the collected id/parent_id values and
// registers the tree, returning its handle.
int64_t FinalizeBuffer(FromBuffer& buf) {
  auto t = std::make_unique<Tree>();
  uint32_t n =
      buf.cols.empty() ? 0 : static_cast<uint32_t>(buf.cols[0].is_null.size());
  t->row_count = n;
  t->columns = std::move(buf.cols);
  // Map id value -> row index (last writer wins on duplicate ids, matching a
  // self-join lookup). NULL ids are not addressable as parents.
  std::unordered_map<int64_t, uint32_t> id_to_row;
  id_to_row.reserve(n);
  for (uint32_t r = 0; r < n; ++r) {
    if (!t->columns[0].is_null[r]) {
      id_to_row[t->columns[0].i64[r]] = r;
    }
  }
  t->parent.resize(n, Tree::kNullParent);
  for (uint32_t r = 0; r < n; ++r) {
    if (buf.parent_id_null[r]) {
      continue;
    }
    auto it = id_to_row.find(buf.parent_id_val[r]);
    t->parent[r] = it == id_to_row.end() ? Tree::kNullParent : it->second;
  }
  return HandleRegistry<Tree>::Instance().Insert(std::move(t));
}

void FromFinalize(duckdb_function_info,
                  duckdb_aggregate_state* source,
                  duckdb_vector result,
                  idx_t count,
                  idx_t offset) {
  auto* out = static_cast<int64_t*>(duckdb_vector_get_data(result));
  for (idx_t i = 0; i < count; ++i) {
    FromState& slot = *reinterpret_cast<FromState*>(source[i]);
    std::unique_ptr<FromBuffer> buf(slot ? slot : new FromBuffer());
    slot = nullptr;
    out[offset + i] = FinalizeBuffer(*buf);
  }
}

void FromDestroy(duckdb_aggregate_state* states, idx_t count) {
  for (idx_t i = 0; i < count; ++i) {
    FromState& slot = *reinterpret_cast<FromState*>(states[i]);
    delete slot;
    slot = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Scalar helpers: read a single ANY/typed value into a tree::Constraint value.
// ---------------------------------------------------------------------------
std::optional<tree::Op> ParseOp(std::string_view op) {
  if (op == "=" || op == "==") {
    return tree::Op::kEq;
  }
  if (op == "!=" || op == "<>") {
    return tree::Op::kNe;
  }
  if (op == "<") {
    return tree::Op::kLt;
  }
  if (op == "<=") {
    return tree::Op::kLe;
  }
  if (op == ">") {
    return tree::Op::kGt;
  }
  if (op == ">=") {
    return tree::Op::kGe;
  }
  std::string lower = base::ToLower(std::string(op));
  if (lower == "glob") {
    return tree::Op::kGlob;
  }
  if (lower == "is null") {
    return tree::Op::kIsNull;
  }
  if (lower == "is not null") {
    return tree::Op::kIsNotNull;
  }
  return std::nullopt;
}

// __intrinsic_tree_constraint(column VARCHAR, op VARCHAR, value ANY) -> BIGINT.
void ConstraintExec(duckdb_function_info info,
                    duckdb_data_chunk in,
                    duckdb_vector out) {
  idx_t n = duckdb_data_chunk_get_size(in);
  duckdb_vector col_vec = duckdb_data_chunk_get_vector(in, 0);
  duckdb_vector op_vec = duckdb_data_chunk_get_vector(in, 1);
  duckdb_vector val_vec = duckdb_data_chunk_get_vector(in, 2);
  duckdb_logical_type val_type = duckdb_vector_get_column_type(val_vec);
  duckdb_type val_tid = duckdb_get_type_id(val_type);
  duckdb_destroy_logical_type(&val_type);
  auto* res = static_cast<int64_t*>(duckdb_vector_get_data(out));
  for (idx_t row = 0; row < n; ++row) {
    auto c = std::make_unique<Constraint>();
    c->column = std::string(ReadVarchar(col_vec, row));
    std::optional<tree::Op> op = ParseOp(ReadVarchar(op_vec, row));
    c->op = op.value_or(tree::Op::kEq);
    c->value_type = MapColType(val_tid);
    if (!IsRowNull(val_vec, row)) {
      switch (c->value_type) {
        case ColType::kInt64:
          c->i64 = ReadInt64(val_vec, val_tid, row);
          break;
        case ColType::kDouble:
          c->f64 = ReadDouble(val_vec, val_tid, row);
          break;
        case ColType::kString:
          c->str = std::string(ReadVarchar(val_vec, row));
          break;
      }
    }
    res[row] = HandleRegistry<Constraint>::Instance().Insert(std::move(c));
  }
  (void)info;
}

// __intrinsic_tree_where_and(BIGINT...) -> BIGINT. Takes each constraint handle
// and bundles them into a constraint vector handle.
void WhereAndExec(duckdb_function_info,
                  duckdb_data_chunk in,
                  duckdb_vector out) {
  idx_t n = duckdb_data_chunk_get_size(in);
  idx_t argc = duckdb_data_chunk_get_column_count(in);
  std::vector<duckdb_vector> args(argc);
  for (idx_t a = 0; a < argc; ++a) {
    args[a] = duckdb_data_chunk_get_vector(in, a);
  }
  auto* res = static_cast<int64_t*>(duckdb_vector_get_data(out));
  for (idx_t row = 0; row < n; ++row) {
    auto vec = std::make_unique<std::vector<Constraint>>();
    for (idx_t a = 0; a < argc; ++a) {
      if (IsRowNull(args[a], row)) {
        continue;
      }
      int64_t h = ReadInt64(args[a], DUCKDB_TYPE_BIGINT, row);
      std::unique_ptr<Constraint> c =
          HandleRegistry<Constraint>::Instance().Take(h);
      if (c) {
        vec->push_back(std::move(*c));
      }
    }
    res[row] = HandleRegistry<std::vector<Constraint>>::Instance().Insert(
        std::move(vec));
  }
}

// __intrinsic_tree_filter(tree BIGINT, where BIGINT) -> BIGINT.
void FilterExec(duckdb_function_info, duckdb_data_chunk in, duckdb_vector out) {
  idx_t n = duckdb_data_chunk_get_size(in);
  duckdb_vector tree_vec = duckdb_data_chunk_get_vector(in, 0);
  duckdb_vector where_vec = duckdb_data_chunk_get_vector(in, 1);
  auto* res = static_cast<int64_t*>(duckdb_vector_get_data(out));
  duckdb_vector_ensure_validity_writable(out);
  uint64_t* out_valid = duckdb_vector_get_validity(out);
  for (idx_t row = 0; row < n; ++row) {
    if (IsRowNull(tree_vec, row) || IsRowNull(where_vec, row)) {
      duckdb_validity_set_row_invalid(out_valid, row);
      continue;
    }
    std::unique_ptr<Tree> t = HandleRegistry<Tree>::Instance().Take(
        ReadInt64(tree_vec, DUCKDB_TYPE_BIGINT, row));
    std::unique_ptr<std::vector<Constraint>> w =
        HandleRegistry<std::vector<Constraint>>::Instance().Take(
            ReadInt64(where_vec, DUCKDB_TYPE_BIGINT, row));
    if (!t || !w) {
      duckdb_validity_set_row_invalid(out_valid, row);
      continue;
    }
    base::StatusOr<Tree> filtered = tree::FilterTree(*t, *w);
    if (!filtered.ok()) {
      duckdb_validity_set_row_invalid(out_valid, row);
      continue;
    }
    res[row] = HandleRegistry<Tree>::Instance().Insert(
        std::make_unique<Tree>(std::move(*filtered)));
  }
}

// __intrinsic_tree_propagate_down(tree BIGINT, spec VARCHAR...) -> BIGINT.
void PropagateExec(duckdb_function_info,
                   duckdb_data_chunk in,
                   duckdb_vector out) {
  idx_t n = duckdb_data_chunk_get_size(in);
  idx_t argc = duckdb_data_chunk_get_column_count(in);
  duckdb_vector tree_vec = duckdb_data_chunk_get_vector(in, 0);
  std::vector<duckdb_vector> spec_vecs;
  for (idx_t a = 1; a < argc; ++a) {
    spec_vecs.push_back(duckdb_data_chunk_get_vector(in, a));
  }
  auto* res = static_cast<int64_t*>(duckdb_vector_get_data(out));
  duckdb_vector_ensure_validity_writable(out);
  uint64_t* out_valid = duckdb_vector_get_validity(out);
  for (idx_t row = 0; row < n; ++row) {
    if (IsRowNull(tree_vec, row)) {
      duckdb_validity_set_row_invalid(out_valid, row);
      continue;
    }
    std::unique_ptr<Tree> t = HandleRegistry<Tree>::Instance().Take(
        ReadInt64(tree_vec, DUCKDB_TYPE_BIGINT, row));
    if (!t) {
      duckdb_validity_set_row_invalid(out_valid, row);
      continue;
    }
    std::vector<tree::PropagateSpec> specs;
    bool ok = true;
    for (duckdb_vector sv : spec_vecs) {
      if (IsRowNull(sv, row)) {
        continue;
      }
      std::optional<tree::PropagateSpec> ps =
          tree::ParsePropagateSpec(std::string(ReadVarchar(sv, row)));
      if (!ps) {
        ok = false;
        break;
      }
      specs.push_back(*ps);
    }
    base::StatusOr<Tree> propagated =
        ok ? tree::PropagateDown(*t, specs) : base::ErrStatus("bad spec");
    if (!ok || !propagated.ok()) {
      duckdb_validity_set_row_invalid(out_valid, row);
      continue;
    }
    res[row] = HandleRegistry<Tree>::Instance().Insert(
        std::make_unique<Tree>(std::move(*propagated)));
  }
}

// ---------------------------------------------------------------------------
// __intrinsic_tree_to_table(tree BIGINT, name VARCHAR...) -> LIST<STRUCT>.
// ---------------------------------------------------------------------------

// The output STRUCT has 4 fixed BIGINT columns (c0.._tree_id, c1.._tree_parent
// _id, c2..id, c3..parent_id) followed by 6 type-erased UNION passthrough slots
// (c4..c9). `name...` selects which tree columns fill c4.. in order.
constexpr idx_t kFixedCols = 4;
constexpr idx_t kMaxPassthrough = 6;
constexpr idx_t kStructCols = kFixedCols + kMaxPassthrough;  // 10.

// Writes a tree::Column value (or NULL) into the UNION slot at element `e`.
struct UnionParts {
  duckdb_vector tag;
  duckdb_vector m_i;
  duckdb_vector m_d;
  duckdb_vector m_s;
  uint8_t* tag_data;
  int64_t* i_data;
  double* d_data;
  uint64_t* union_valid;
  uint64_t* vi;
  uint64_t* vd;
  uint64_t* vs;
};

UnionParts GetUnionParts(duckdb_vector union_vec) {
  UnionParts p;
  p.tag = duckdb_struct_vector_get_child(union_vec, 0);
  p.m_i = duckdb_struct_vector_get_child(union_vec, kIntM + 1);
  p.m_d = duckdb_struct_vector_get_child(union_vec, kDoubleM + 1);
  p.m_s = duckdb_struct_vector_get_child(union_vec, kStringM + 1);
  p.tag_data = static_cast<uint8_t*>(duckdb_vector_get_data(p.tag));
  p.i_data = static_cast<int64_t*>(duckdb_vector_get_data(p.m_i));
  p.d_data = static_cast<double*>(duckdb_vector_get_data(p.m_d));
  duckdb_vector_ensure_validity_writable(union_vec);
  duckdb_vector_ensure_validity_writable(p.m_i);
  duckdb_vector_ensure_validity_writable(p.m_d);
  duckdb_vector_ensure_validity_writable(p.m_s);
  p.union_valid = duckdb_vector_get_validity(union_vec);
  p.vi = duckdb_vector_get_validity(p.m_i);
  p.vd = duckdb_vector_get_validity(p.m_d);
  p.vs = duckdb_vector_get_validity(p.m_s);
  return p;
}

void SetUnionNull(const UnionParts& p, idx_t e) {
  p.tag_data[e] = kIntM;
  duckdb_validity_set_row_invalid(p.vi, e);
  duckdb_validity_set_row_invalid(p.vd, e);
  duckdb_validity_set_row_invalid(p.vs, e);
  duckdb_validity_set_row_invalid(p.union_valid, e);
}

void SetUnionValue(const UnionParts& p,
                   idx_t e,
                   const Column& col,
                   uint32_t r) {
  // Non-active members default NULL so a downstream flatten never reads
  // garbage.
  duckdb_validity_set_row_invalid(p.vi, e);
  duckdb_validity_set_row_invalid(p.vd, e);
  duckdb_validity_set_row_invalid(p.vs, e);
  if (col.is_null[r]) {
    SetUnionNull(p, e);
    return;
  }
  switch (col.type) {
    case ColType::kInt64:
      p.tag_data[e] = kIntM;
      p.i_data[e] = col.i64[r];
      duckdb_validity_set_row_valid(p.vi, e);
      break;
    case ColType::kDouble:
      p.tag_data[e] = kDoubleM;
      p.d_data[e] = col.f64[r];
      duckdb_validity_set_row_valid(p.vd, e);
      break;
    case ColType::kString:
      p.tag_data[e] = kStringM;
      duckdb_vector_assign_string_element_len(p.m_s, e, col.str[r].data(),
                                              col.str[r].size());
      duckdb_validity_set_row_valid(p.vs, e);
      break;
  }
}

void ToTableExec(duckdb_function_info,
                 duckdb_data_chunk in,
                 duckdb_vector out) {
  idx_t n = duckdb_data_chunk_get_size(in);
  idx_t argc = duckdb_data_chunk_get_column_count(in);
  duckdb_vector handle_vec = duckdb_data_chunk_get_vector(in, 0);
  std::vector<duckdb_vector> name_vecs;
  for (idx_t a = 1; a < argc; ++a) {
    name_vecs.push_back(duckdb_data_chunk_get_vector(in, a));
  }

  // Take each input row's tree; compute total node count for the flat child.
  std::vector<std::unique_ptr<Tree>> trees(n);
  idx_t total = 0;
  for (idx_t row = 0; row < n; ++row) {
    if (!IsRowNull(handle_vec, row)) {
      trees[row] = HandleRegistry<Tree>::Instance().Take(
          ReadInt64(handle_vec, DUCKDB_TYPE_BIGINT, row));
    }
    if (trees[row]) {
      total += trees[row]->row_count;
    }
  }

  duckdb_list_vector_reserve(out, total);
  duckdb_list_vector_set_size(out, total);
  duckdb_vector struct_vec = duckdb_list_vector_get_child(out);
  duckdb_vector cols[kStructCols];
  for (idx_t c = 0; c < kStructCols; ++c) {
    cols[c] = duckdb_struct_vector_get_child(struct_vec, c);
  }
  auto* c0 = static_cast<int64_t*>(duckdb_vector_get_data(cols[0]));
  auto* c1 = static_cast<int64_t*>(duckdb_vector_get_data(cols[1]));
  auto* c2 = static_cast<int64_t*>(duckdb_vector_get_data(cols[2]));
  auto* c3 = static_cast<int64_t*>(duckdb_vector_get_data(cols[3]));
  duckdb_vector_ensure_validity_writable(cols[1]);  // _tree_parent_id nullable.
  duckdb_vector_ensure_validity_writable(cols[3]);  // parent_id nullable.
  uint64_t* c1_valid = duckdb_vector_get_validity(cols[1]);
  uint64_t* c3_valid = duckdb_vector_get_validity(cols[3]);
  UnionParts pt[kMaxPassthrough];
  for (idx_t k = 0; k < kMaxPassthrough; ++k) {
    pt[k] = GetUnionParts(cols[kFixedCols + k]);
  }

  auto* entries = static_cast<duckdb_list_entry*>(duckdb_vector_get_data(out));
  idx_t cursor = 0;
  for (idx_t row = 0; row < n; ++row) {
    entries[row].offset = cursor;
    entries[row].length = trees[row] ? trees[row]->row_count : 0;
    if (!trees[row]) {
      continue;
    }
    const Tree& t = *trees[row];
    // Resolve which tree column feeds each requested passthrough slot.
    int slot_col[kMaxPassthrough];
    idx_t num_names = name_vecs.size();
    for (idx_t k = 0; k < kMaxPassthrough; ++k) {
      slot_col[k] = -1;
      if (k < num_names && !IsRowNull(name_vecs[k], row)) {
        std::string nm(ReadVarchar(name_vecs[k], row));
        for (size_t c = 0; c < t.columns.size(); ++c) {
          if (t.columns[c].name == nm) {
            slot_col[k] = static_cast<int>(c);
            break;
          }
        }
      }
    }
    for (uint32_t r = 0; r < t.row_count; ++r) {
      c0[cursor] = r;
      if (t.parent[r] == Tree::kNullParent) {
        duckdb_validity_set_row_invalid(c1_valid, cursor);
      } else {
        c1[cursor] = t.parent[r];
      }
      // id (col 0) and parent_id (col 1) are always present in the tree.
      c2[cursor] = t.columns[0].i64[r];
      if (t.columns[1].is_null[r]) {
        duckdb_validity_set_row_invalid(c3_valid, cursor);
      } else {
        c3[cursor] = t.columns[1].i64[r];
      }
      for (idx_t k = 0; k < kMaxPassthrough; ++k) {
        if (slot_col[k] < 0) {
          SetUnionNull(pt[k], cursor);
        } else {
          SetUnionValue(pt[k], cursor,
                        t.columns[static_cast<size_t>(slot_col[k])], r);
        }
      }
      ++cursor;
    }
  }
}

// ---------------------------------------------------------------------------
// Registration.
// ---------------------------------------------------------------------------
base::Status RegisterScalar(duckdb_connection conn,
                            const char* name,
                            const std::vector<duckdb_logical_type>& params,
                            duckdb_logical_type ret,
                            duckdb_scalar_function_t fn,
                            std::optional<duckdb_logical_type> varargs) {
  duckdb_scalar_function f = duckdb_create_scalar_function();
  duckdb_scalar_function_set_name(f, name);
  for (duckdb_logical_type p : params) {
    duckdb_scalar_function_add_parameter(f, p);
  }
  if (varargs) {
    duckdb_scalar_function_set_varargs(f, *varargs);
  }
  duckdb_scalar_function_set_return_type(f, ret);
  // Tree handles carry state across calls; never constant-fold them away.
  duckdb_scalar_function_set_special_handling(f);
  duckdb_scalar_function_set_function(f, fn);
  duckdb_state st = duckdb_register_scalar_function(conn, f);
  duckdb_destroy_scalar_function(&f);
  if (st == DuckDBError) {
    return base::ErrStatus("RegisterTreeFunctions: failed to register '%s'",
                           name);
  }
  return base::OkStatus();
}

}  // namespace

base::Status RegisterTreeFunctions(duckdb_connection conn) {
  duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
  duckdb_logical_type varchar = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
  duckdb_logical_type any = duckdb_create_logical_type(DUCKDB_TYPE_ANY);

  duckdb_logical_type varchar_list = duckdb_create_list_type(varchar);
  // from_table aggregate: (id BIGINT, parent_id BIGINT, names VARCHAR[],
  // values ROW/STRUCT ANY) -> handle. Names and values are parallel; the values
  // struct is positional (DuckDB named-struct syntax uses `:=`, which the
  // SQLite-grammar tokenizer rejects in a macro body).
  base::Status agg_status;
  {
    duckdb_aggregate_function f = duckdb_create_aggregate_function();
    duckdb_aggregate_function_set_name(f, "__intrinsic_tree_from_table");
    duckdb_aggregate_function_add_parameter(f, bigint);
    duckdb_aggregate_function_add_parameter(f, bigint);
    duckdb_aggregate_function_add_parameter(f, varchar_list);
    duckdb_aggregate_function_add_parameter(f, any);
    duckdb_aggregate_function_set_return_type(f, bigint);
    duckdb_aggregate_function_set_functions(
        f, FromStateSize, FromInit, FromUpdate, FromCombine, FromFinalize);
    duckdb_aggregate_function_set_destructor(f, FromDestroy);
    if (duckdb_register_aggregate_function(conn, f) == DuckDBError) {
      agg_status = base::ErrStatus(
          "RegisterTreeFunctions: from_table registration failed");
    }
    duckdb_destroy_aggregate_function(&f);
  }

  // The UNION(i BIGINT, d DOUBLE, s VARCHAR) passthrough element type.
  duckdb_logical_type u_members[3] = {
      bigint, duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE), varchar};
  const char* u_names[3] = {"i", "d", "s"};
  duckdb_logical_type union_type =
      duckdb_create_union_type(u_members, u_names, 3);
  duckdb_destroy_logical_type(&u_members[1]);

  // to_table return type: LIST<STRUCT(c0..c3 BIGINT, c4..c9 UNION)>.
  duckdb_logical_type struct_members[kStructCols];
  const char* struct_names[kStructCols] = {"c0", "c1", "c2", "c3", "c4",
                                           "c5", "c6", "c7", "c8", "c9"};
  for (idx_t c = 0; c < kFixedCols; ++c) {
    struct_members[c] = bigint;
  }
  for (idx_t c = kFixedCols; c < kStructCols; ++c) {
    struct_members[c] = union_type;
  }
  duckdb_logical_type to_struct =
      duckdb_create_struct_type(struct_members, struct_names, kStructCols);
  duckdb_logical_type to_list = duckdb_create_list_type(to_struct);

  // The constraint/where/filter/propagate_down functions are DELEGATES-TO
  // PerfettoSQL functions that are NOT macro-expanded, so they reach DuckDB
  // under their surface names; register those directly. to_table/from_table use
  // the __intrinsic_* names referenced by the macro overrides.
  base::Status s;
  if (s = agg_status; s.ok()) {
    s = RegisterScalar(conn, "_tree_constraint", {varchar, varchar, any},
                       bigint, ConstraintExec, std::nullopt);
  }
  if (s.ok()) {
    s = RegisterScalar(conn, "_tree_where", {}, bigint, WhereAndExec, bigint);
  }
  if (s.ok()) {
    s = RegisterScalar(conn, "_tree_filter", {bigint, bigint}, bigint,
                       FilterExec, std::nullopt);
  }
  if (s.ok()) {
    s = RegisterScalar(conn, "_tree_propagate_down", {bigint}, bigint,
                       PropagateExec, varchar);
  }
  if (s.ok()) {
    s = RegisterScalar(conn, "__intrinsic_tree_to_table", {bigint}, to_list,
                       ToTableExec, varchar);
  }

  duckdb_destroy_logical_type(&to_list);
  duckdb_destroy_logical_type(&to_struct);
  duckdb_destroy_logical_type(&union_type);
  duckdb_destroy_logical_type(&varchar_list);
  duckdb_destroy_logical_type(&any);
  duckdb_destroy_logical_type(&varchar);
  duckdb_destroy_logical_type(&bigint);
  return s;
}

}  // namespace perfetto::trace_processor::duckdb_integration
