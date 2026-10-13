// SQL generation for the operator macros (sql/*.sql).
//
// A macro like pf_interval_flatten(rel, aggs, per) expands to
// `FROM query(pf__sql_interval_flatten(rel, aggs, per, threads))`: the
// generator below runs once at bind time (constant folding) and returns the
// SQL text of the kernel pipeline. Building the text in C++ keeps binding in
// the microseconds; the equivalent SQL-macro string building took 5-100 ms
// per query.
//
// Per-trace contract: `per` lists the partition columns, trace first (e.g.
// ['_trace_uuid', 'utid']). The leading columns become the GROUP BY of the
// kernel aggregate (so traces are solved independently and in parallel);
// for interval operators the last column is the lane, handled inside the
// kernel (it must be an integer column).
#include "common.h"  // no-include-violation-check

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace {

using Strs = std::vector<std::string>;

std::string Lower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos)
    return "";
  size_t b = s.find_last_not_of(" \t\r\n;");
  return s.substr(a, b - a + 1);
}

std::string Join(const Strs& v, const std::string& sep) {
  std::string r;
  for (size_t i = 0; i < v.size(); i++)
    r += (i ? sep : "") + v[i];
  return r;
}

// A relation argument: a table / view name, or a query (wrapped in parens).
std::string Src(const std::string& r) {
  std::string t = Trim(r);
  if (t.empty())
    return t;
  std::string inner = t;
  if (inner.size() >= 2 && inner.front() == '(' && inner.back() == ')') {
    int depth = 0;
    bool q = false;
    bool wraps = true;
    for (size_t i = 0; i < inner.size(); i++) {
      if (inner[i] == '\'')
        q = !q;
      else if (!q && inner[i] == '(')
        depth++;
      else if (!q && inner[i] == ')') {
        depth--;
        if (depth == 0 && i + 1 < inner.size()) {
          wraps = false;
          break;
        }
      }
    }
    if (wraps)
      inner = Trim(inner.substr(1, inner.size() - 2));
  }
  std::string li = Lower(inner);
  auto starts_inner = [&](const char* kw) {
    size_t n = strlen(kw);
    return li.compare(0, n, kw) == 0 &&
           (li.size() == n || !std::isalnum(static_cast<unsigned char>(li[n])));
  };
  if (inner.find("|>") != std::string::npos || starts_inner("interval") ||
      starts_inner("intervals") || starts_inner("graph")) {
    return "(" + CompilePipeSql(inner, {}, 1) + ")";
  }
  std::string l = Lower(t);
  auto starts = [&](const char* kw) {
    size_t n = strlen(kw);
    return l.compare(0, n, kw) == 0 &&
           (l.size() == n || !std::isalnum(static_cast<unsigned char>(l[n])));
  };
  if (t[0] == '(' || starts("select") || starts("with") || starts("from") ||
      starts("values"))
    return "(" + t + ")";
  return t;
}

// "expr AS name" -> {expr, name}; empty name when malformed.
std::pair<std::string, std::string> SplitAs(const std::string& a) {
  std::string t = Trim(a);
  std::string l = Lower(t);
  for (size_t p = l.rfind(" as "); p != std::string::npos;
       p = p ? l.rfind(" as ", p - 1) : std::string::npos) {
    std::string name = Trim(t.substr(p + 4));
    bool ident = !name.empty();
    for (char c : name)
      ident &= std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    if (ident)
      return {Trim(t.substr(0, p)), name};
    if (!p)
      break;
  }
  return {t, ""};
}

// Aggregate specs: each element may itself list several comma-separated
// aggregates ('count(*) AS c, sum(v) AS s').
Strs SplitAggs(const Strs& aggs) {
  Strs out;
  for (const auto& a : aggs) {
    int depth = 0;
    bool quoted = false;
    size_t start = 0;
    for (size_t i = 0; i <= a.size(); i++) {
      char c = i < a.size() ? a[i] : ',';
      if (c == '\'')
        quoted = !quoted;
      else if (!quoted && c == '(')
        depth++;
      else if (!quoted && c == ')')
        depth--;
      else if (!quoted && depth == 0 && c == ',') {
        std::string t = Trim(a.substr(start, i - start));
        if (!t.empty())
          out.push_back(t);
        start = i + 1;
      }
    }
  }
  return out;
}

// "fn(args)" -> {lower(fn), args}; fn empty when not a call.
std::pair<std::string, std::string> SplitCall(const std::string& e) {
  size_t a = e.find('(');
  if (a == std::string::npos || e.back() != ')')
    return {"", ""};
  return {Lower(Trim(e.substr(0, a))), Trim(e.substr(a + 1, e.size() - a - 2))};
}

std::string Error(const std::string& msg) {
  std::string q = msg;
  size_t p = 0;
  while ((p = q.find('\'', p)) != std::string::npos) {
    q.insert(p, "'");
    p += 2;
  }
  return "SELECT error('" + q + "')";
}

struct Per {
  Strs groups;       // GROUP BY columns (trace first)
  std::string lane;  // lane column, empty if none
};
bool IsIntLaneCol(const std::string& col) {
  std::string c = Lower(Trim(col));
  static const char* const kIntLanes[] = {
      "utid", "upid",      "cpu",    "track_id",    "id",         "tid",
      "pid",  "parent_id", "gpu_id", "cookie",      "machine_id", "session_id",
      "lane", "key",       "k",      "callsite_id", "frame_id",   "symbol_id"};
  for (const char* k : kIntLanes) {
    if (c == k)
      return true;
  }
  return false;
}

// Interval operators: the last of >= 2 columns is the lane when it is an
// integer lane key; non-integer keys stay in GROUP BY.
Per IntervalPer(const Strs& per) {
  Per p;
  p.groups = per;
  if (per.size() >= 2 && IsIntLaneCol(per.back())) {
    p.lane = per.back();
    p.groups.pop_back();
  }
  return p;
}

// GROUP BY clause. With threads > 1 a constant extra key keeps DuckDB off
// its perfect-hash aggregate, whose finalize is single threaded; the radix
// hash aggregate finalizes partitions (traces) in parallel.
std::string GroupBy(const Strs& g, int64_t threads) {
  if (g.empty())
    return "";
  return " GROUP BY " + Join(g, ", ") + (threads > 1 ? ", true" : "");
}

std::string Prefixed(const Strs& cols, const std::string& pre) {
  std::string r;
  for (const auto& c : cols)
    r += pre + c + ", ";
  return r;
}

// ---------------------------------------------------------------------------
// INTERVAL FLATTEN
// ---------------------------------------------------------------------------
std::string IntervalFlatten(const std::string& rel,
                            const Strs& aggs,
                            const Strs& per,
                            int64_t threads) {
  // Kernel path: COUNT(*), COUNT(x), SUM(x), AVG(x) as up to 8 SUM inputs.
  Strs inputs, outs;
  bool native = true;
  for (const auto& a : aggs) {
    auto [expr, name] = SplitAs(a);
    if (name.empty())
      return Error(
          "pf_interval_flatten: aggregate must look like 'expr AS name': " + a);
    auto [fn, arg] = SplitCall(expr);
    auto input = [&](const std::string& x) {
      inputs.push_back(x);
      return "f.s" + std::to_string(inputs.size() - 1);
    };
    if (fn == "count" && arg == "*") {
      outs.push_back("f.cnt AS " + name);
    } else if (fn == "count") {
      outs.push_back("coalesce(" +
                     input("((" + arg + ") IS NOT NULL)::BIGINT") + ", 0) AS " +
                     name);
    } else if (fn == "sum") {
      outs.push_back(input("(" + arg + ")::BIGINT") + " AS " + name);
    } else if (fn == "avg") {
      std::string s = input("(" + arg + ")::BIGINT");
      std::string n = input("((" + arg + ") IS NOT NULL)::BIGINT");
      outs.push_back(s + " / nullif(" + n + ", 0) AS " + name);
    } else {
      native = false;
    }
  }
  if (inputs.size() > 8)
    native = false;
  Per p = IntervalPer(per);
  if (native) {
    std::string lane = p.lane.empty() ? "0" : "(" + p.lane + ")::BIGINT";
    std::string args = lane + ", ts::BIGINT, dur::BIGINT";
    for (const auto& x : inputs)
      args += ", " + x;
    std::string head =
        "SELECT f.ts AS ts, f.dur AS dur, " + Prefixed(p.groups, "") +
        (p.lane.empty() ? "" : "f.lane AS " + p.lane + ", ") + Join(outs, ", ");
    if (outs.empty() && head.size() >= 2 &&
        head.substr(head.size() - 2) == ", ") {
      head.resize(head.size() - 2);
    }
    return head + " FROM (SELECT " + Prefixed(p.groups, "") +
           "unnest(pf_interval_flatten_lane_agg(" + args + ")) AS f FROM " +
           Src(rel) + " __pf_t" + GroupBy(p.groups, threads) + ")";
  }
  // Any other aggregate: explode the segments' rows and aggregate in SQL.
  return "WITH o AS MATERIALIZED (SELECT pf_row_id() AS __pf_r, * "
         "FROM " +
         Src(rel) + " __pf_t), s AS (SELECT " + Prefixed(per, "") +
         "unnest(pf_interval_flatten_agg(0, __pf_r, ts::BIGINT, dur::BIGINT)) "
         "AS f FROM o" +
         GroupBy(per, threads) +
         "), x AS (SELECT f.ts AS __pf_ts, f.dur AS __pf_dur, unnest(f.rows) "
         "AS __pf_r FROM s) "
         "SELECT __pf_ts AS ts, __pf_dur AS dur, " +
         Prefixed(per, "") + Join(aggs, ", ") +
         " FROM x JOIN o USING (__pf_r) GROUP BY " + Prefixed(per, "") +
         "__pf_ts, __pf_dur";
}

std::string OnKeys(const Strs& per,
                   const std::string& a,
                   const std::string& b) {
  std::string r;
  for (const auto& c : per)
    r += a + "." + c + " = " + b + "." + c + " AND ";
  return r;
}

// ---------------------------------------------------------------------------
// INTERVAL INTERSECTION OF (rels AS aliases) PER per
// ---------------------------------------------------------------------------
std::string IntervalIntersection(const Strs& rels,
                                 const Strs& aliases,
                                 const Strs& per,
                                 bool full_row,
                                 int64_t threads,
                                 const std::vector<Strs>& needed_cols = {}) {
  if (rels.size() < 2 || rels.size() != aliases.size())
    return Error(
        "pf_interval_intersection: needs >= 2 relations and one alias each");
  Per p = IntervalPer(per);
  std::string lane = p.lane.empty() ? "0" : "(" + p.lane + ")::BIGINT";
  size_t n = rels.size();
  // Native kernel for up to 4 operands; ids are the operands' `id` columns,
  // or lock-free C++ `pf_row_id()` handles when payload columns are joined.
  if (n <= 4) {
    std::string with, feed, joins;
    Strs cols;
    for (size_t i = 0; i < n; i++) {
      std::string src = Src(rels[i]);
      bool has_needed = (needed_cols.size() == n);
      bool only_id_or_none =
          !full_row ||
          (has_needed &&
           (needed_cols[i].empty() ||
            (needed_cols[i].size() == 1 && Lower(needed_cols[i][0]) == "id")));
      if (only_id_or_none) {
        std::string id_expr = (full_row && has_needed && needed_cols[i].empty())
                                  ? "0::BIGINT"
                                  : "id::BIGINT";
        feed += (i ? " UNION ALL " : "") + std::string("SELECT ") +
                Prefixed(p.groups, "") + lane + " AS __pf_lane, " +
                std::to_string(i) + "::INTEGER AS __pf_side, " + id_expr +
                " AS __pf_id, ts::BIGINT AS __pf_ts, dur::BIGINT AS __pf_dur "
                "FROM " +
                src + " __pf_t";
        cols.push_back("{'id': f.r" + std::to_string(i) + "} AS " + aliases[i]);
      } else {
        std::string struct_expr = "__pf_t";
        if (has_needed &&
            !(needed_cols[i].size() == 1 && needed_cols[i][0] == "*")) {
          Strs fields;
          for (const auto& c : needed_cols[i]) {
            fields.push_back("'" + c + "': __pf_t." + c);
          }
          struct_expr = "{" + Join(fields, ", ") + "}";
        }
        std::string o = "__pf_o" + std::to_string(i);
        with += (with.empty() ? "WITH " : ", ") + o +
                " AS MATERIALIZED (SELECT " + Prefixed(p.groups, "") + lane +
                " AS __pf_lane, pf_row_id() AS __pf_r, ts::BIGINT AS __pf_ts, "
                "dur::BIGINT AS __pf_dur, " +
                struct_expr + " AS __pf_s FROM " + src + " __pf_t)";
        feed += (i ? " UNION ALL " : "") + std::string("SELECT ") +
                Prefixed(p.groups, "") + "__pf_lane, " + std::to_string(i) +
                "::INTEGER AS __pf_side, __pf_r AS __pf_id, __pf_ts, __pf_dur "
                "FROM " +
                o;
        cols.push_back(o + ".__pf_s AS " + aliases[i]);
        joins +=
            " JOIN " + o + " ON " + o + ".__pf_r = f.r" + std::to_string(i);
      }
    }
    std::string k = "(SELECT " + Prefixed(p.groups, "") +
                    "unnest(pf_interval_intersect" + std::to_string(n) +
                    "_lane_agg(__pf_lane, __pf_side, __pf_id, __pf_ts, "
                    "__pf_dur)) AS f FROM (" +
                    feed + ")" + GroupBy(p.groups, threads) + ") __pf_k";
    std::string sel = "SELECT f.ts AS ts, f.dur AS dur, " +
                      Prefixed(p.groups, "__pf_k.") +
                      (p.lane.empty() ? "" : "f.lane AS " + p.lane + ", ");
    return with + (with.empty() ? "" : " ") + sel + Join(cols, ", ") +
           " FROM " + k + joins;
  }
  // More operands: generic kernel with a row list per fragment.
  std::string with, feed;
  for (size_t i = 0; i < n; i++) {
    with += (i ? ", " : "WITH ") + std::string("__pf_o") + std::to_string(i) +
            " AS MATERIALIZED (SELECT " + Prefixed(per, "") +
            "pf_row_id() AS __pf_r, ts::BIGINT AS ts, dur::BIGINT AS dur, "
            "__pf_t AS __pf_s FROM " +
            Src(rels[i]) + " __pf_t)";
    feed += (i ? " UNION ALL " : "") + std::string("SELECT ") +
            Prefixed(per, "") + std::to_string(i) +
            "::INTEGER AS side, __pf_r AS row, ts, dur FROM __pf_o" +
            std::to_string(i);
  }
  std::string sel;
  std::string joins;
  for (size_t i = 0; i < n; i++) {
    std::string o = "__pf_o" + std::to_string(i);
    sel += ", " + (full_row ? o + ".__pf_s" : "{'id': " + o + ".__pf_s.id}") +
           " AS " + aliases[i];
    joins += " JOIN " + o + " ON " + o + ".__pf_r = f.rows[" +
             std::to_string(i + 1) + "]";
  }
  return with + ", __pf_k AS (SELECT " + Prefixed(per, "") +
         "unnest(pf_interval_intersect_agg(side, row, ts, dur, " +
         std::to_string(n) + ")) AS f FROM (" + feed + ")" +
         GroupBy(per, threads) + ") SELECT f.ts AS ts, f.dur AS dur, " +
         Prefixed(per, "__pf_k.") + sel.substr(2) + " FROM __pf_k" + joins;
}

// ---------------------------------------------------------------------------
// Trees: the partition is the whole `per` list.
// ---------------------------------------------------------------------------
std::string Quote(const std::string& x) {
  std::string q = "'";
  for (char c : x)
    q += c == '\'' ? std::string("''") : std::string(1, c);
  return q + "'";
}

std::string QuoteList(const Strs& v) {
  Strs q;
  for (const auto& x : v)
    q.push_back(Quote(x));
  return "[" + Join(q, ", ") + "]::VARCHAR[]";
}

// TREE ACCUMULATE (UP | DOWN) aggs. SUM aggregates run the native kernel
// (one per aggregate, zipped by unnest: every kernel returns its rows in id
// order); the output is (per..., id, aggs...), or every input column plus
// the aggregates with full_row. Other aggregates use pf_tree__accumulate_full.
std::string TreeAccumulate(const std::string& tbl,
                           const std::string& direction,
                           const Strs& aggs,
                           const Strs& per,
                           bool full_row,
                           int64_t threads) {
  std::string d = Lower(direction);
  if (d != "up" && d != "down")
    return Error("pf_tree: direction must be 'up' or 'down'");
  Strs kernels, outs;
  for (const auto& a : aggs) {
    auto [expr, name] = SplitAs(a);
    auto [fn, arg] = SplitCall(expr);
    if (name.empty() || fn != "sum" || arg.empty()) {
      return "SELECT * FROM pf_tree__accumulate_full(" + Quote(tbl) + ", " +
             Quote(direction) + ", " + QuoteList(aggs) +
             ", per := " + QuoteList(per) + ")";
    }
    std::string f = "__pf_f" + std::to_string(kernels.size());
    kernels.push_back("unnest(pf_tree_accumulate_sum_agg(id, parent_id, (" +
                      arg + "), " + (d == "up" ? "0" : "1") + ")) AS " + f);
    outs.push_back(f + ".v AS " + name);
  }
  if (kernels.empty())
    return Error("pf_tree_accumulate: needs at least one aggregate");
  std::string lean = "SELECT " + Prefixed(per, "") + "__pf_f0.id AS id, " +
                     Join(outs, ", ") + " FROM (SELECT " + Prefixed(per, "") +
                     Join(kernels, ", ") + " FROM " + Src(tbl) + " __pf_t" +
                     GroupBy(per, threads) + ")";
  if (!full_row)
    return lean;
  Strs names;
  for (const auto& a : aggs)
    names.push_back("__pf_k." + SplitAs(a).second);
  return "SELECT __pf_t.*, " + Join(names, ", ") + " FROM " + Src(tbl) +
         " __pf_t JOIN (" + lean + ") __pf_k ON " +
         OnKeys(per, "__pf_t", "__pf_k") + "__pf_t.id = __pf_k.id";
}

// TREE KEEP / DROP IF (ANCESTOR | DESCENDANT) OF marker, TREE PRUNE AT
// marker. One kernel pass over the tree rows and the marker ids (fed
// together); output (per..., id, parent_id) of the surviving rows, or the
// surviving input rows with full_row. With `over` (a filtered working set
// tested against the full structure) the SQL-join implementation is used.
std::string TreeFilter(const std::string& op,
                       const std::string& tbl,
                       const std::string& marker,
                       const Strs& per,
                       const std::string& over,
                       bool full_row,
                       int64_t threads) {
  int mode;
  bool keep;
  if (op == "keep_descendant") {
    mode = 0, keep = true;
  } else if (op == "keep_ancestor") {
    mode = 1, keep = true;
  } else if (op == "drop_descendant") {
    mode = 0, keep = false;
  } else if (op == "drop_ancestor") {
    mode = 1, keep = false;
  } else if (op == "prune") {
    mode = 2, keep = false;
  } else {
    return Error("pf_tree: relation must be 'ancestor' or 'descendant'");
  }
  if (!over.empty()) {
    return "SELECT * FROM query(pf_tree__flag_filter(" + Quote(tbl) + ", " +
           Quote(marker) + ", " + QuoteList(per) + ", " + Quote(over) + ", '" +
           std::to_string(mode) + "', " + (keep ? "true" : "false") + "))";
  }
  std::string k = Prefixed(per, "");
  std::string lean =
      "SELECT " + k +
      "__pf_f.id AS id, __pf_f.parent_id AS parent_id FROM (SELECT " + k +
      "unnest(pf_tree_filter_agg(id, parent_id, __pf_mk, " +
      std::to_string(mode | (keep ? 0 : 4)) + ")) AS __pf_f FROM (SELECT " + k +
      "id::BIGINT AS id, parent_id::BIGINT AS parent_id, false AS __pf_mk "
      "FROM " +
      Src(tbl) + " __pf_t UNION ALL SELECT " + k +
      "id::BIGINT, NULL::BIGINT, true FROM " + Src(marker) + " __pf_m)" +
      GroupBy(per, threads) + ")";
  if (!full_row)
    return lean;
  return "SELECT __pf_t.* FROM " + Src(tbl) + " __pf_t SEMI JOIN (" + lean +
         ") __pf_k ON " + OnKeys(per, "__pf_t", "__pf_k") +
         "__pf_t.id = __pf_k.id";
}

// ---------------------------------------------------------------------------
// GRAPH (DFS | BFS | DOMINATOR) TREE NODES nodes EDGES edges FROM seeds.
// Nodes, edges and seeds are fed to one kernel call per partition; output
// (per..., id, parent_id, depth), plus the node payload with full_row.
// ---------------------------------------------------------------------------
std::string GraphTree(const std::string& kind,
                      const std::string& nodes,
                      const std::string& edges,
                      const std::string& seeds,
                      const Strs& per,
                      bool full_row,
                      int64_t threads) {
  std::string k = Lower(kind) == "dfs" ? "0" : Lower(kind) == "bfs" ? "1" : "2";
  std::string p = Prefixed(per, "");
  std::string lean =
      "SELECT " + p +
      "__pf_f.id AS id, __pf_f.parent_id AS parent_id, "
      "__pf_f.depth AS depth FROM (SELECT " +
      p + "unnest(pf_graph_tree_agg(" + k +
      ", __pf_role, __pf_a, __pf_b)) AS __pf_f FROM (SELECT " + p +
      "0 AS __pf_role, id::BIGINT AS __pf_a, NULL::BIGINT AS __pf_b FROM " +
      Src(nodes) + " __pf_n UNION ALL SELECT " + p +
      "1, src::BIGINT, dst::BIGINT FROM " + Src(edges) +
      " __pf_e UNION ALL SELECT " + p + "2, id::BIGINT, NULL FROM " +
      Src(seeds) + " __pf_s)" + GroupBy(per, threads) + ")";
  if (!full_row)
    return lean;
  // Payload columns named parent_id / depth are replaced by the tree's.
  Strs ex = per;
  ex.push_back("id");
  return "SELECT __pf_k.*, __pf_p.* EXCLUDE (" + Join(ex, ", ") + ") FROM (" +
         lean +
         ") __pf_k JOIN (SELECT COLUMNS(c -> c NOT IN ('parent_id', "
         "'depth')) FROM " +
         Src(nodes) + " __pf_n0) __pf_p ON " + OnKeys(per, "__pf_k", "__pf_p") +
         "__pf_k.id = __pf_p.id";
}

// ---------------------------------------------------------------------------
// Scalar function plumbing: arguments are VARCHAR or VARCHAR[] (lists may be
// given as a single string), BOOLEAN or BIGINT.
// ---------------------------------------------------------------------------
std::string Str(duckdb_string_t* s) {
  return std::string(duckdb_string_t_data(s), duckdb_string_t_length(*s));
}

Strs ReadStrs(duckdb_vector v, idx_t row) {
  uint64_t* val = duckdb_vector_get_validity(v);
  if (val && !duckdb_validity_row_is_valid(val, row))
    return {};
  duckdb_logical_type t = duckdb_vector_get_column_type(v);
  duckdb_type id = duckdb_get_type_id(t);
  duckdb_destroy_logical_type(&t);
  if (id == DUCKDB_TYPE_VARCHAR) {
    std::string s = Trim(
        Str(static_cast<duckdb_string_t*>(duckdb_vector_get_data(v)) + row));
    return s.empty() ? Strs{} : Strs{s};
  }
  auto* e = static_cast<duckdb_list_entry*>(duckdb_vector_get_data(v)) + row;
  duckdb_vector child = duckdb_list_vector_get_child(v);
  auto* cs = static_cast<duckdb_string_t*>(duckdb_vector_get_data(child));
  Strs r;
  for (idx_t i = e->offset; i < e->offset + e->length; i++)
    r.push_back(Trim(Str(cs + i)));
  return r;
}

struct Args {
  duckdb_data_chunk chunk;
  idx_t row;
  Strs strs(idx_t c) const {
    return ReadStrs(duckdb_data_chunk_get_vector(chunk, c), row);
  }
  std::string str(idx_t c) const {
    Strs s = strs(c);
    return s.empty() ? "" : s[0];
  }
  int64_t i64(idx_t c) const {
    duckdb_vector v = duckdb_data_chunk_get_vector(chunk, c);
    duckdb_logical_type t = duckdb_vector_get_column_type(v);
    duckdb_type id = duckdb_get_type_id(t);
    duckdb_destroy_logical_type(&t);
    if (id == DUCKDB_TYPE_BOOLEAN)
      return static_cast<bool*>(duckdb_vector_get_data(v))[row];
    if (id == DUCKDB_TYPE_VARCHAR)
      return std::atoll(str(c).c_str());
    return static_cast<int64_t*>(duckdb_vector_get_data(v))[row];
  }
};

using Gen = std::string (*)(const Args&);

template <Gen G>
void Run(duckdb_function_info, duckdb_data_chunk input, duckdb_vector out) {
  idx_t n = duckdb_data_chunk_get_size(input);
  for (idx_t r = 0; r < n; r++) {
    std::string sql = G(Args{input, r});
    duckdb_vector_assign_string_element_len(out, r, sql.data(), sql.size());
  }
}

std::string GenFlatten(const Args& a) {
  return IntervalFlatten(a.str(0), SplitAggs(a.strs(1)), a.strs(2), a.i64(3));
}
std::string GenIntersection(const Args& a) {
  return IntervalIntersection(a.strs(0), a.strs(1), a.strs(2), a.i64(3) != 0,
                              a.i64(4));
}

std::string GenTreeAccumulate(const Args& a) {
  return TreeAccumulate(a.str(0), a.str(1), SplitAggs(a.strs(2)), a.strs(3),
                        a.i64(4) != 0, a.i64(5));
}
std::string GenTreeFilter(const Args& a) {
  return TreeFilter(Lower(a.str(0)), a.str(1), a.str(2), a.strs(3), a.str(4),
                    a.i64(5) != 0, a.i64(6));
}

std::string GenGraphTree(const Args& a) {
  return GraphTree(a.str(0), a.str(1), a.str(2), a.str(3), a.strs(4),
                   a.i64(5) != 0, a.i64(6));
}

std::string GenPipe(const Args& a) {
  return CompilePipeSql(a.str(0), a.strs(1), a.i64(2));
}

enum Kind { kStr, kList, kBool, kInt };

// Registers `name` with every VARCHAR / VARCHAR[] combination of the kList
// parameters.
bool RegisterGen(duckdb_connection conn,
                 const char* name,
                 const std::vector<Kind>& params,
                 duckdb_scalar_function_t fn) {
  duckdb_scalar_function_set set = duckdb_create_scalar_function_set(name);
  std::vector<size_t> lists;
  for (size_t i = 0; i < params.size(); i++)
    if (params[i] == kList)
      lists.push_back(i);
  bool ok = true;
  for (size_t mask = 0; mask < (size_t{1} << lists.size()); mask++) {
    duckdb_scalar_function f = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(f, name);
    for (size_t i = 0, li = 0; i < params.size(); i++) {
      duckdb_logical_type t;
      if (params[i] == kList) {
        duckdb_logical_type v = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
        t = (mask >> li++ & 1)
                ? duckdb_create_list_type(v)
                : duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
        duckdb_destroy_logical_type(&v);
      } else {
        t = duckdb_create_logical_type(params[i] == kStr ? DUCKDB_TYPE_VARCHAR
                                       : params[i] == kBool
                                           ? DUCKDB_TYPE_BOOLEAN
                                           : DUCKDB_TYPE_BIGINT);
      }
      duckdb_scalar_function_add_parameter(f, t);
      duckdb_destroy_logical_type(&t);
    }
    duckdb_logical_type ret = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    duckdb_scalar_function_set_return_type(f, ret);
    duckdb_destroy_logical_type(&ret);
    duckdb_scalar_function_set_special_handling(f);
    duckdb_scalar_function_set_function(f, fn);
    ok &= duckdb_add_scalar_function_to_set(set, f) == DuckDBSuccess;
    duckdb_destroy_scalar_function(&f);
  }
  ok &= duckdb_register_scalar_function_set(conn, set) == DuckDBSuccess;
  duckdb_destroy_scalar_function_set(&set);
  return ok;
}

void RunRowId(duckdb_function_info,
              duckdb_data_chunk input,
              duckdb_vector out) {
  static std::atomic<int64_t> next_id{0};
  idx_t n = duckdb_data_chunk_get_size(input);
  pf::Progress::AddRowIdRows(n);
  int64_t base =
      next_id.fetch_add(static_cast<int64_t>(n), std::memory_order_relaxed);
  int64_t* dst = static_cast<int64_t*>(duckdb_vector_get_data(out));
  for (idx_t i = 0; i < n; i++)
    dst[i] = base + static_cast<int64_t>(i);
}

bool RegisterRowId(duckdb_connection conn) {
  duckdb_scalar_function f = duckdb_create_scalar_function();
  duckdb_scalar_function_set_name(f, "pf_row_id");
  duckdb_logical_type ret = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
  duckdb_scalar_function_set_return_type(f, ret);
  duckdb_destroy_logical_type(&ret);
  duckdb_scalar_function_set_volatile(f);
  duckdb_scalar_function_set_function(f, RunRowId);
  bool ok = (duckdb_register_scalar_function(conn, f) == DuckDBSuccess);
  duckdb_destroy_scalar_function(&f);
  return ok;
}

}  // namespace

std::string GenIntervalFlattenSql(const std::string& rel,
                                  const std::vector<std::string>& aggs,
                                  const std::vector<std::string>& per,
                                  int64_t threads) {
  return IntervalFlatten(rel, SplitAggs(aggs), per, threads);
}

std::string GenIntervalIntersectionSql(
    const std::vector<std::string>& rels,
    const std::vector<std::string>& aliases,
    const std::vector<std::string>& per,
    bool full_row,
    int64_t threads,
    const std::vector<std::vector<std::string>>& needed_cols) {
  return IntervalIntersection(rels, aliases, per, full_row, threads,
                              needed_cols);
}

std::string GenTreeAccumulateSql(const std::string& tbl,
                                 const std::string& direction,
                                 const std::vector<std::string>& aggs,
                                 const std::vector<std::string>& per,
                                 bool full_row,
                                 int64_t threads) {
  return TreeAccumulate(tbl, direction, SplitAggs(aggs), per, full_row,
                        threads);
}

std::string GenTreeFilterSql(const std::string& op,
                             const std::string& tbl,
                             const std::string& marker,
                             const std::vector<std::string>& per,
                             const std::string& over,
                             bool full_row,
                             int64_t threads) {
  return TreeFilter(Lower(op), tbl, marker, per, over, full_row, threads);
}

std::string GenGraphTreeSql(const std::string& kind,
                            const std::string& nodes,
                            const std::string& edges,
                            const std::string& seeds,
                            const std::vector<std::string>& per,
                            bool full_row,
                            int64_t threads) {
  return GraphTree(kind, nodes, edges, seeds, per, full_row, threads);
}

bool RegisterSqlGenFunctions(duckdb_connection conn) {
  return RegisterRowId(conn) &&
         RegisterGen(conn, "pf__sql_interval_flatten",
                     {kStr, kList, kList, kInt}, Run<GenFlatten>) &&
         RegisterGen(conn, "pf__sql_interval_intersection",
                     {kList, kList, kList, kBool, kInt},
                     Run<GenIntersection>) &&
         RegisterGen(conn, "pf__sql_tree_accumulate",
                     {kStr, kStr, kList, kList, kBool, kInt},
                     Run<GenTreeAccumulate>) &&
         RegisterGen(conn, "pf__sql_tree_filter",
                     {kStr, kStr, kStr, kList, kStr, kBool, kInt},
                     Run<GenTreeFilter>) &&
         RegisterGen(conn, "pf__sql_graph_tree",
                     {kStr, kStr, kStr, kStr, kList, kBool, kInt},
                     Run<GenGraphTree>) &&
         RegisterGen(conn, "pf__sql_pipe", {kStr, kList, kInt}, Run<GenPipe>);
}
