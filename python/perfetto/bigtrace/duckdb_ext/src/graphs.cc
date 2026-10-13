// Graph operators (spec §7): GRAPH ( DFS | BFS | DOMINATOR ) TREE.
//
// Native kernel, one aggregate function so DuckDB's parallel hash aggregate
// does the PER partitioning and each group (one graph) is solved in one go:
//
//   pf_graph_tree_agg(kind INTEGER, role INTEGER, a BIGINT, b BIGINT)
//     -> STRUCT(id BIGINT, parent_id BIGINT, depth BIGINT)[]
//
//   kind: 0 = DFS, 1 = BFS, 2 = DOMINATOR (taken from the first non-NULL row).
//   role: 0 = node   (a = node id)
//         1 = edge   (a = src, b = dst)
//         2 = seed   (a = node id; FROM rel)
//
// The rows of the three relations are fed as one UNION ALL (see
// sql/graphs.sql). Semantics (identical for the three kinds where relevant):
//
// * The vertex set is the set of distinct non-NULL node ids. Edges with a
//   NULL endpoint or an endpoint that is not a node are ignored; so are seeds
//   that are not nodes. Duplicate nodes / edges / seeds and self loops are
//   harmless (dedup).
// * Ties are broken by node id: successors are visited in ascending dst id,
//   seeds in ascending id. This is exactly TP's legacy
//   graph_reachable_dfs / graph_reachable_bfs when TP is given edges ordered
//   by (src, dst) and start nodes ordered by id DESC (TP's DFS uses a stack).
// * DFS: classic DFS forest: for each seed in ascending id order that is not
//   yet visited, a depth-first traversal rooted at it. A seed reached from an
//   earlier seed is an ordinary tree node (it gets a parent), as in TP.
// * BFS: all seeds are roots (depth 0), then FIFO by level; a node's parent
//   is the first node (in BFS order) with an edge to it, i.e. a shortest-hop
//   tree, as in TP.
// * DOMINATOR: dominator tree (Lengauer-Tarjan, path compression) over the
//   nodes reachable from the seeds. With one seed it is the root. With
//   several seeds a virtual super-root with an edge to every seed is inserted
//   and stripped: nodes immediately dominated by it become roots (parent_id
//   NULL), as in android.memory.heap_graph's heap_graph_dominator_tree.
// * Output: one row per reached node; depth = 0 at the roots.
//
// Graphs (partitions) are solved in parallel by DuckDB itself: the SQL layer
// groups so that the radix hash aggregate finalizes partitions concurrently.
#include "common.h"  // no-include-violation-check

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr int32_t kNone = -1;

// Uninitialized heap array (avoids zero-filling large buffers).
template <typename T>
struct Buf {
  std::unique_ptr<T[]> p;
  void Alloc(size_t n) { p.reset(new T[n]); }
  void Fill(size_t n, T v) {
    Alloc(n);
    std::fill(p.get(), p.get() + n, v);
  }
  T& operator[](size_t i) { return p[i]; }
  const T& operator[](size_t i) const { return p[i]; }
  T* data() { return p.get(); }
  void Reset() { p.reset(); }
};

//===----------------------------------------------------------------------===//
// Aggregate state
//===----------------------------------------------------------------------===//
struct GraphState {
  int32_t kind = -1;
  std::vector<int64_t> nodes;
  std::vector<int64_t> src, dst;
  std::vector<int64_t> seeds;

  template <typename V>
  static void Append(V& a, V& b) {
    if (a.empty()) {
      a.swap(b);
    } else {
      a.insert(a.end(), b.begin(), b.end());
    }
    V().swap(b);
  }
  void Merge(GraphState& o) {
    if (kind < 0)
      kind = o.kind;
    Append(nodes, o.nodes);
    Append(src, o.src);
    Append(dst, o.dst);
    Append(seeds, o.seeds);
  }
};

GraphState*& StatePtr(duckdb_aggregate_state s) {
  return *reinterpret_cast<GraphState**>(s);
}
idx_t StateSize(duckdb_function_info) {
  return sizeof(GraphState*);
}
void StateInit(duckdb_function_info, duckdb_aggregate_state s) {
  StatePtr(s) = nullptr;
}
void StateDestroy(duckdb_aggregate_state* states, idx_t count) {
  for (idx_t i = 0; i < count; i++) {
    delete StatePtr(states[i]);
    StatePtr(states[i]) = nullptr;
  }
}
void StateCombine(duckdb_function_info,
                  duckdb_aggregate_state* source,
                  duckdb_aggregate_state* target,
                  idx_t count) {
  for (idx_t i = 0; i < count; i++) {
    GraphState*& s = StatePtr(source[i]);
    GraphState*& d = StatePtr(target[i]);
    if (!s)
      continue;
    if (!d) {
      d = s;
      s = nullptr;
      continue;
    }
    d->Merge(*s);
    pf::Progress::MergeGroup();
  }
}

template <typename T>
struct Col {
  const T* data;
  uint64_t* validity;
  explicit Col(duckdb_vector v)
      : data(static_cast<const T*>(duckdb_vector_get_data(v))),
        validity(duckdb_vector_get_validity(v)) {}
  bool Valid(idx_t r) const {
    return !validity || duckdb_validity_row_is_valid(validity, r);
  }
};

void GraphUpdate(duckdb_function_info,
                 duckdb_data_chunk input,
                 duckdb_aggregate_state* states) {
  idx_t n = duckdb_data_chunk_get_size(input);
  pf::Progress::AddIngestedRows(n);
  Col<int32_t> kind(duckdb_data_chunk_get_vector(input, 0));
  Col<int32_t> role(duckdb_data_chunk_get_vector(input, 1));
  Col<int64_t> a(duckdb_data_chunk_get_vector(input, 2));
  Col<int64_t> b(duckdb_data_chunk_get_vector(input, 3));
  // Runs of rows with one state and one role (the relations are fed one
  // after another) are appended in bulk.
  for (idx_t r = 0; r < n;) {
    duckdb_aggregate_state sp = states[r];
    int32_t ro = role.Valid(r) ? role.data[r] : -1;
    idx_t e = r + 1;
    while (e < n && states[e] == sp && role.Valid(e) && role.data[e] == ro)
      e++;
    GraphState*& st = StatePtr(sp);
    if (!st) {
      st = new GraphState();
      pf::Progress::AddGroup();
    }
    if (st->kind < 0 && kind.Valid(r))
      st->kind = kind.data[r];
    std::vector<int64_t>* dst = ro == 0   ? &st->nodes
                                : ro == 2 ? &st->seeds
                                          : nullptr;
    for (idx_t i = r; i < e; i++) {
      if (!a.Valid(i))
        continue;
      if (dst) {
        dst->push_back(a.data[i]);
      } else if (ro == 1 && b.Valid(i)) {
        st->src.push_back(a.data[i]);
        st->dst.push_back(b.data[i]);
      }
    }
    r = e;
  }
}

//===----------------------------------------------------------------------===//
// Id -> dense index mapping. Dense index = rank of the id among the distinct
// node ids, so index order == id order (used for tie-breaking).
//===----------------------------------------------------------------------===//
struct IdMap {
  std::vector<int64_t> ids;  // distinct, sorted
  // Dense mode: table[x - lo] = index or kNone.
  bool dense = false;
  int64_t lo = 0;
  uint64_t range = 0;
  Buf<int32_t> table;
  // Hash mode: open addressing.
  uint64_t mask = 0;
  Buf<int64_t> hkey;
  Buf<int32_t> hval;

  static uint64_t Hash(int64_t x) {
    uint64_t h = static_cast<uint64_t>(x) * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
  }

  void Build(std::vector<int64_t>& raw) {
    ids.clear();
    if (raw.empty())
      return;
    int64_t mn = raw[0], mx = raw[0];
    for (int64_t x : raw) {
      mn = std::min(mn, x);
      mx = std::max(mx, x);
    }
    uint64_t r = static_cast<uint64_t>(mx) - static_cast<uint64_t>(mn) + 1;
    if (r != 0 && r <= 4 * static_cast<uint64_t>(raw.size()) + 1024) {
      dense = true;
      lo = mn;
      range = r;
      table.Fill(r, kNone);
      for (int64_t x : raw)
        table[static_cast<uint64_t>(x) - static_cast<uint64_t>(lo)] = 0;
      ids.reserve(raw.size());
      int32_t k = 0;
      for (uint64_t i = 0; i < r; i++) {
        if (table[i] == 0) {
          table[i] = k++;
          ids.push_back(static_cast<int64_t>(static_cast<uint64_t>(lo) + i));
        }
      }
      if (static_cast<uint64_t>(k) == r) {
        identity = true;
        table.Reset();
      }
      return;
    }
    ids = raw;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    uint64_t cap = 16;
    while (cap < 2 * ids.size())
      cap <<= 1;
    mask = cap - 1;
    hkey.Alloc(cap);
    hval.Fill(cap, kNone);
    for (size_t i = 0; i < ids.size(); i++) {
      uint64_t h = Hash(ids[i]) & mask;
      while (hval[h] != kNone)
        h = (h + 1) & mask;
      hkey[h] = ids[i];
      hval[h] = static_cast<int32_t>(i);
    }
  }

  inline int32_t Find(int64_t x) const {
    if (ids.empty())
      return kNone;
    if (identity) {
      uint64_t o = static_cast<uint64_t>(x) - static_cast<uint64_t>(lo);
      return o < range ? static_cast<int32_t>(o) : kNone;
    }
    if (dense) {
      uint64_t o = static_cast<uint64_t>(x) - static_cast<uint64_t>(lo);
      return o < range ? table[o] : kNone;
    }
    for (uint64_t h = Hash(x) & mask;; h = (h + 1) & mask) {
      if (hval[h] == kNone)
        return kNone;
      if (hkey[h] == x)
        return hval[h];
    }
  }

  bool identity = false;
};

struct OutRow {
  int64_t id;
  int64_t parent;  // valid iff has_parent
  int64_t depth;
  bool has_parent;
};

//===----------------------------------------------------------------------===//
// Solver for one graph.
//===----------------------------------------------------------------------===//
struct Solver {
  IdMap map;
  int32_t n = 0;
  // CSR successors sorted by dst index; CSR predecessors (sorted by src).
  Buf<uint32_t> out_off, out_adj, in_off, in_adj;
  std::vector<int32_t> seeds;  // distinct, ascending index

  void Build(GraphState& st) {
    map.Build(st.nodes);
    std::vector<int64_t>().swap(st.nodes);
    n = static_cast<int32_t>(map.ids.size());
    for (int64_t s : st.seeds) {
      int32_t k = map.Find(s);
      if (k != kNone)
        seeds.push_back(k);
    }
    std::vector<int64_t>().swap(st.seeds);
    std::sort(seeds.begin(), seeds.end());
    seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());

    // Build outgoing CSR directly from st.src / st.dst in two passes without
    // allocating extra intermediate edge arrays or global predecessors when
    // unused.
    size_t m0 = st.src.size();
    size_t m = 0;
    out_off.Fill(static_cast<size_t>(n) + 1, 0);
    if (st.kind == 2) {
      in_off.Fill(static_cast<size_t>(n) + 1, 0);
      for (size_t i = 0; i < m0; i++) {
        int32_t s = map.Find(st.src[i]);
        if (s == kNone)
          continue;
        int32_t d = map.Find(st.dst[i]);
        if (d == kNone)
          continue;
        out_off[s + 1]++;
        in_off[d + 1]++;
        m++;
      }
      for (int32_t i = 0; i < n; i++) {
        out_off[i + 1] += out_off[i];
        in_off[i + 1] += in_off[i];
      }
      out_adj.Alloc(m);
      in_adj.Alloc(m);
      Buf<uint32_t> fill_out, fill_in;
      fill_out.Alloc(n);
      fill_in.Alloc(n);
      std::memcpy(fill_out.data(), out_off.data(), sizeof(uint32_t) * n);
      std::memcpy(fill_in.data(), in_off.data(), sizeof(uint32_t) * n);
      for (size_t i = 0; i < m0; i++) {
        int32_t s = map.Find(st.src[i]);
        if (s == kNone)
          continue;
        int32_t d = map.Find(st.dst[i]);
        if (d == kNone)
          continue;
        out_adj[fill_out[s]++] = static_cast<uint32_t>(d);
        in_adj[fill_in[d]++] = static_cast<uint32_t>(s);
      }
    } else {
      for (size_t i = 0; i < m0; i++) {
        int32_t s = map.Find(st.src[i]);
        if (s == kNone)
          continue;
        int32_t d = map.Find(st.dst[i]);
        if (d == kNone)
          continue;
        out_off[s + 1]++;
        m++;
      }
      for (int32_t i = 0; i < n; i++)
        out_off[i + 1] += out_off[i];
      out_adj.Alloc(m);
      Buf<uint32_t> fill_out;
      fill_out.Alloc(n);
      std::memcpy(fill_out.data(), out_off.data(), sizeof(uint32_t) * n);
      for (size_t i = 0; i < m0; i++) {
        int32_t s = map.Find(st.src[i]);
        if (s == kNone)
          continue;
        int32_t d = map.Find(st.dst[i]);
        if (d == kNone)
          continue;
        out_adj[fill_out[s]++] = static_cast<uint32_t>(d);
      }
    }
    std::vector<int64_t>().swap(st.src);
    std::vector<int64_t>().swap(st.dst);
  }

  inline void SortAdj(int32_t v) {
    uint32_t b = out_off[v], e = out_off[v + 1];
    if (e - b > 1)
      std::sort(out_adj.data() + b, out_adj.data() + e);
  }

  // DFS forest (recursive order emulated with an explicit stack of edge
  // cursors). Fills order (preorder of node indices) and par (by index).
  // If `virtual_root` the roots are the children of a virtual node.
  void Dfs(std::vector<int32_t>& order, Buf<int32_t>& par) {
    par.Fill(n, kNone - 1);  // kNone - 1 = unvisited
    order.reserve(n);
    struct Frame {
      int32_t v;
      uint32_t cur;
    };
    std::vector<Frame> stack;
    for (int32_t s : seeds) {
      if (par[s] != kNone - 1)
        continue;
      par[s] = kNone;
      order.push_back(s);
      SortAdj(s);
      stack.push_back({s, out_off[s]});
      while (!stack.empty()) {
        Frame& f = stack.back();
        if (f.cur == out_off[f.v + 1]) {
          stack.pop_back();
          continue;
        }
        int32_t w = static_cast<int32_t>(out_adj[f.cur++]);
        if (par[w] != kNone - 1)
          continue;
        par[w] = f.v;
        order.push_back(w);
        SortAdj(w);
        stack.push_back({w, out_off[w]});
      }
    }
  }

  void EmitTree(const std::vector<int32_t>& order,
                const Buf<int32_t>& par,
                const Buf<int32_t>& depth,
                std::vector<OutRow>& out) {
    out.resize(order.size());
    for (size_t i = 0; i < order.size(); i++) {
      int32_t v = order[i];
      int32_t p = par[v];
      out[i] = {map.ids[v], p >= 0 ? map.ids[p] : 0, depth[v], p >= 0};
    }
  }

  void SolveDfs(std::vector<OutRow>& out) {
    std::vector<int32_t> order;
    Buf<int32_t> par, depth;
    Dfs(order, par);
    depth.Alloc(n);
    for (int32_t v : order)
      depth[v] = par[v] >= 0 ? depth[par[v]] + 1 : 0;
    EmitTree(order, par, depth, out);
  }

  void SolveBfs(std::vector<OutRow>& out) {
    Buf<int32_t> par, depth;
    par.Fill(n, kNone - 1);
    depth.Alloc(n);
    std::vector<int32_t> q;
    q.reserve(n);
    for (int32_t s : seeds) {
      par[s] = kNone;
      depth[s] = 0;
      q.push_back(s);
    }
    for (size_t h = 0; h < q.size(); h++) {
      int32_t v = q[h];
      SortAdj(v);
      for (uint32_t k = out_off[v]; k < out_off[v + 1]; k++) {
        int32_t w = static_cast<int32_t>(out_adj[k]);
        if (par[w] != kNone - 1)
          continue;
        par[w] = v;
        depth[w] = depth[v] + 1;
        q.push_back(w);
      }
    }
    EmitTree(q, par, depth, out);
  }

  // Lengauer-Tarjan with path compression ("simple" version, O(m log n)),
  // all per-vertex arrays indexed by DFS preorder number. With several seeds,
  // preorder number 0 is a virtual super-root whose children are the seeds.
  void SolveDominator(std::vector<OutRow>& out) {
    if (seeds.empty())
      return;
    const bool virt = seeds.size() > 1;
    std::vector<int32_t> order;
    Buf<int32_t> dfs_par;
    Dfs(order, dfs_par);
    const int32_t off = virt ? 1 : 0;
    const int32_t N = static_cast<int32_t>(order.size()) + off;
    Buf<int32_t> pre;  // node index -> preorder number, kNone if unreached
    pre.Fill(n, kNone);
    for (size_t i = 0; i < order.size(); i++)
      pre[order[i]] = static_cast<int32_t>(i) + off;
    Buf<int32_t> par, semi, label, anc, dom, bhead, bnext;
    par.Alloc(N);
    semi.Alloc(N);
    label.Alloc(N);
    anc.Fill(N, kNone);
    dom.Fill(N, kNone);
    bhead.Fill(N, kNone);
    bnext.Alloc(N);
    Buf<uint8_t> is_seed;
    if (virt) {
      is_seed.Fill(n, 0);
      for (int32_t s : seeds)
        is_seed[s] = 1;
      par[0] = kNone;
    }
    for (int32_t k = 0; k < N; k++) {
      semi[k] = k;
      label[k] = k;
    }
    for (size_t i = 0; i < order.size(); i++) {
      int32_t v = order[i];
      int32_t p = dfs_par[v];
      par[i + off] = p >= 0 ? pre[p] : (virt ? 0 : kNone);
    }
    std::vector<int32_t> path;
    auto eval = [&](int32_t v) -> int32_t {
      if (anc[v] == kNone)
        return v;
      if (anc[anc[v]] != kNone) {
        path.clear();
        int32_t x = v;
        while (anc[anc[x]] != kNone) {
          path.push_back(x);
          x = anc[x];
        }
        for (size_t j = path.size(); j-- > 0;) {
          int32_t y = path[j];
          int32_t a = anc[y];
          if (semi[label[a]] < semi[label[y]])
            label[y] = label[a];
          anc[y] = anc[a];
        }
      }
      return label[v];
    };
    for (int32_t w = N - 1; w > 0; w--) {
      int32_t node = order[w - off];
      for (uint32_t k = in_off[node]; k < in_off[node + 1]; k++) {
        int32_t pv = pre[in_adj[k]];
        if (pv == kNone)
          continue;
        int32_t u = eval(pv);
        if (semi[u] < semi[w])
          semi[w] = semi[u];
      }
      if (virt && is_seed[node]) {
        // Edge from the virtual root (preorder 0): semi candidate 0.
        semi[w] = 0;
      }
      bnext[w] = bhead[semi[w]];
      bhead[semi[w]] = w;
      int32_t pw = par[w];
      anc[w] = pw;
      for (int32_t v = bhead[pw]; v != kNone; v = bnext[v]) {
        int32_t u = eval(v);
        dom[v] = semi[u] < semi[v] ? u : pw;
      }
      bhead[pw] = kNone;
    }
    for (int32_t w = 1; w < N; w++)
      if (dom[w] != semi[w])
        dom[w] = dom[dom[w]];
    // depth by preorder (dom[w] < w).
    Buf<int32_t> depth;
    depth.Alloc(N);
    depth[0] = virt ? -1 : 0;
    for (int32_t w = 1; w < N; w++)
      depth[w] = depth[dom[w]] + 1;
    out.resize(order.size());
    for (int32_t w = off; w < N; w++) {
      int32_t d = dom[w];
      bool hp = d != kNone && !(virt && d == 0);
      out[w - off] = {map.ids[order[w - off]], hp ? map.ids[order[d - off]] : 0,
                      depth[w], hp};
    }
  }

  void Solve(GraphState& st, std::vector<OutRow>& out) {
    Build(st);
    if (n == 0 || seeds.empty())
      return;
    switch (st.kind) {
      case 1:
        SolveBfs(out);
        break;
      case 2:
        SolveDominator(out);
        break;
      default:
        SolveDfs(out);
        break;
    }
  }
};

void GraphFinalize(duckdb_function_info info,
                   duckdb_aggregate_state* source,
                   duckdb_vector result,
                   idx_t count,
                   idx_t offset) {
  std::vector<std::vector<OutRow>> res(count);
  for (idx_t i = 0; i < count; i++) {
    GraphState* st = StatePtr(source[i]);
    if (!st)
      continue;
    const char* err = nullptr;
    if (st->kind < 0 || st->kind > 2)
      err = "pf_graph_tree_agg: kind must be 0 (DFS), 1 (BFS) or 2 (DOMINATOR)";
    else if (st->nodes.size() >= static_cast<size_t>(INT32_MAX))
      err = "pf_graph_tree_agg: too many nodes in one graph";
    if (err) {
      duckdb_aggregate_function_set_error(info, err);
      return;
    }
    Solver s;
    s.Solve(*st, res[i]);
    pf::Progress::FinishGroup();
  }
  auto* entries =
      static_cast<duckdb_list_entry*>(duckdb_vector_get_data(result));
  idx_t base = duckdb_list_vector_get_size(result);
  idx_t total = 0;
  std::vector<idx_t> offs(count);
  for (idx_t i = 0; i < count; i++) {
    offs[i] = base + total;
    total += res[i].size();
  }
  if (duckdb_list_vector_reserve(result, base + total) == DuckDBError ||
      duckdb_list_vector_set_size(result, base + total) == DuckDBError) {
    duckdb_aggregate_function_set_error(info,
                                        "pf_graph_tree_agg: out of memory");
    return;
  }
  duckdb_vector child = duckdb_list_vector_get_child(result);
  duckdb_vector cid = duckdb_struct_vector_get_child(child, 0);
  duckdb_vector cpar = duckdb_struct_vector_get_child(child, 1);
  duckdb_vector cdep = duckdb_struct_vector_get_child(child, 2);
  duckdb_vector_ensure_validity_writable(cpar);
  auto* did = static_cast<int64_t*>(duckdb_vector_get_data(cid));
  auto* dpar = static_cast<int64_t*>(duckdb_vector_get_data(cpar));
  auto* ddep = static_cast<int64_t*>(duckdb_vector_get_data(cdep));
  uint64_t* vpar = duckdb_vector_get_validity(cpar);
  for (idx_t i = 0; i < count; i++) {
    entries[offset + i].offset = offs[i];
    entries[offset + i].length = res[i].size();
  }
  // Validity bits are shared 64-bit words: write them serially.
  for (idx_t i = 0; i < count; i++) {
    const auto& v = res[i];
    idx_t o = offs[i];
    for (size_t k = 0; k < v.size(); k++) {
      did[o + k] = v[k].id;
      dpar[o + k] = v[k].parent;
      ddep[o + k] = v[k].depth;
      if (v[k].has_parent) {
        duckdb_validity_set_row_valid(vpar, o + k);
      } else {
        duckdb_validity_set_row_invalid(vpar, o + k);
      }
    }
  }
}

}  // namespace

bool RegisterGraphFunctions(duckdb_connection conn) {
  duckdb_logical_type big = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
  duckdb_logical_type i32 = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
  duckdb_logical_type members[3] = {big, big, big};
  const char* names[3] = {"id", "parent_id", "depth"};
  duckdb_logical_type st = duckdb_create_struct_type(members, names, 3);
  duckdb_logical_type ret = duckdb_create_list_type(st);

  duckdb_aggregate_function f = duckdb_create_aggregate_function();
  duckdb_aggregate_function_set_name(f, "pf_graph_tree_agg");
  duckdb_aggregate_function_add_parameter(f, i32);
  duckdb_aggregate_function_add_parameter(f, i32);
  duckdb_aggregate_function_add_parameter(f, big);
  duckdb_aggregate_function_add_parameter(f, big);
  duckdb_aggregate_function_set_return_type(f, ret);
  duckdb_aggregate_function_set_functions(f, StateSize, StateInit, GraphUpdate,
                                          StateCombine, GraphFinalize);
  duckdb_aggregate_function_set_destructor(f, StateDestroy);
  duckdb_aggregate_function_set_special_handling(f);
  bool ok = duckdb_register_aggregate_function(conn, f) == DuckDBSuccess;
  duckdb_destroy_aggregate_function(&f);
  duckdb_destroy_logical_type(&ret);
  duckdb_destroy_logical_type(&st);
  duckdb_destroy_logical_type(&i32);
  duckdb_destroy_logical_type(&big);
  return ok;
}
