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
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"

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
    double rhs = c.value_type == ColType::kInt64 ? static_cast<double>(c.i64)
                                                 : c.f64;
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
        dst.i64[v] = spec.op == AggOp::kSum  ? a + b
                     : spec.op == AggOp::kMin ? std::min(a, b)
                                              : std::max(a, b);
      } else {
        double a = dst.f64[p], b = dst.f64[v];
        dst.f64[v] = spec.op == AggOp::kSum  ? a + b
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
  std::string src =
      base::TrimWhitespace(spec.substr(lp + 1, rp - lp - 1));
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

base::Status RegisterTreeFunctions(duckdb_connection) {
  // TODO: DuckDB bindings (from_table aggregate, constraint/where/filter/
  // propagate scalars, to_table combiner) wire the tree::* core above into the
  // DuckDB catalog. Landing the algorithm core first.
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::duckdb_integration
