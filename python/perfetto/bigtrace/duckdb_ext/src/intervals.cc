// Interval operators (spec §5) for the perfetto DuckDB extension.
//
// Every kernel is an aggregate function: the SQL macro layer
// (sql/intervals.sql) numbers each operand's rows, tags them with an operand
// index ("side"), and GROUPs BY the PER columns, so DuckDB's parallel hash
// aggregate does the partitioning and each group is solved by one sort + sweep
// here. Kernels return LIST<STRUCT> of bounds plus operand row numbers; the
// macros unnest that and join the rows back to recover payload columns.
//
// Interval semantics (identical to TP's PerfettoSQL Next): a row with
// dur > 0 covers [ts, ts + dur); dur = 0 is a point at ts which intersects an
// interval covering it (start <= ts < end) or a point at the same ts. Rows
// with NULL ts or dur are skipped; ts < 0 or dur < 0 is an error.

#include "common.h"  // no-include-violation-check

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr int64_t kNone = -1;

struct Rec {
  int64_t s;     // start
  int64_t e;     // end (s + dur)
  int64_t ep;    // effective end for overlap tests: max(e, s + 1)
  int64_t row;   // operand row number
  int32_t side;  // operand index
};

inline bool ByStart(const Rec& a, const Rec& b) {
  if (a.s != b.s)
    return a.s < b.s;
  if (a.e != b.e)
    return a.e < b.e;
  return a.row < b.row;
}

// Two records overlap under PerfettoSQL point semantics.
inline bool Overlaps(const Rec& a, const Rec& b) {
  return a.s < b.ep && b.s < a.ep;
}

struct Span16 {
  int64_t s;
  int64_t e;
};

struct Rec24 {
  int64_t s;
  int64_t e;
  int64_t row;
};

inline bool ByStart24(const Rec24& a, const Rec24& b) {
  if (a.s != b.s)
    return a.s < b.s;
  if (a.e != b.e)
    return a.e < b.e;
  return a.row < b.row;
}

//===----------------------------------------------------------------------===//
// Thread-local bump arena so 50,000 per-track groups do ZERO malloc/free calls.
//===----------------------------------------------------------------------===//
template <typename T>
struct ArenaVec {
  T* data = nullptr;
  uint32_t size = 0;
  uint32_t cap = 0;

  ArenaVec() = default;
  void clear() {
    std::free(data);
    data = nullptr;
    size = 0;
    cap = 0;
  }
  ArenaVec(const ArenaVec&) = delete;
  ArenaVec& operator=(const ArenaVec&) = delete;
  ArenaVec(ArenaVec&& o) noexcept : data(o.data), size(o.size), cap(o.cap) {
    o.data = nullptr;
    o.size = 0;
    o.cap = 0;
  }
  ArenaVec& operator=(ArenaVec&& o) noexcept {
    if (this != &o) {
      std::free(data);
      data = o.data;
      size = o.size;
      cap = o.cap;
      o.data = nullptr;
      o.size = 0;
      o.cap = 0;
    }
    return *this;
  }
  ~ArenaVec() { std::free(data); }

  void Reserve(uint32_t need) {
    if (need <= cap)
      return;
    uint32_t new_cap = cap ? cap * 2 : 8;
    while (new_cap < need)
      new_cap *= 2;
    data = static_cast<T*>(std::realloc(data, size_t(new_cap) * sizeof(T)));
    cap = new_cap;
  }

  void Push(const T& v) {
    if (size == cap)
      Reserve(size + 1);
    data[size++] = v;
  }

  void push_back(const T& v) { Push(v); }
};

//===----------------------------------------------------------------------===//
// State plumbing: lightweight move-only POD arrays (zero cross-thread
// aliasing).
//===----------------------------------------------------------------------===//
struct State {
  ArenaVec<Span16> spans;  // fast path for FlattenSum0 / MergeCount
  bool spans_sorted = true;
  int64_t last_span_s = std::numeric_limits<int64_t>::min();

  ArenaVec<Rec24> sides[2];  // fast side-separated storage for N=2
  bool side_sorted[2] = {true, true};
  int64_t last_side_s[2] = {std::numeric_limits<int64_t>::min(),
                            std::numeric_limits<int64_t>::min()};
  int64_t min_s[2] = {std::numeric_limits<int64_t>::max(),
                      std::numeric_limits<int64_t>::max()};
  int64_t max_e[2] = {std::numeric_limits<int64_t>::min(),
                      std::numeric_limits<int64_t>::min()};

  std::vector<Rec> recs;
  size_t total_recs = 0;
  int64_t param = 0;  // operand count / relationship code
  std::vector<State*> parts;

  void MaterializeRecs() {
    if (sides[0].size == 0 && sides[1].size == 0)
      return;
    recs.reserve(recs.size() + sides[0].size + sides[1].size);
    for (int sd = 0; sd < 2; sd++) {
      for (uint32_t i = 0; i < sides[sd].size; i++) {
        const Rec24& x = sides[sd].data[i];
        int64_t ep = x.e > x.s ? x.e : x.s + 1;
        recs.push_back({x.s, x.e, ep, x.row, sd});
      }
      sides[sd].size = 0;
    }
  }

  void Merge(State& o) {
    total_recs += o.total_recs;
    o.total_recs = 0;
    param = std::max(param, o.param);
    if (o.spans.size > 0 && recs.empty() && o.recs.empty() && parts.empty() &&
        o.parts.empty()) {
      if (spans.size > 0 && o.spans.data[0].s < last_span_s)
        spans_sorted = false;
      spans_sorted &= o.spans_sorted;
      spans.Reserve(spans.size + o.spans.size);
      std::memcpy(spans.data + spans.size, o.spans.data,
                  size_t(o.spans.size) * sizeof(Span16));
      spans.size += o.spans.size;
      last_span_s = o.last_span_s;
      o.spans.size = 0;
      return;
    }
    if ((o.sides[0].size > 0 || o.sides[1].size > 0) && recs.empty() &&
        o.recs.empty() && parts.empty() && o.parts.empty()) {
      for (int sd = 0; sd < 2; sd++) {
        if (o.sides[sd].size == 0)
          continue;
        if (sides[sd].size > 0 && o.sides[sd].data[0].s < last_side_s[sd])
          side_sorted[sd] = false;
        side_sorted[sd] &= o.side_sorted[sd];
        min_s[sd] = std::min(min_s[sd], o.min_s[sd]);
        max_e[sd] = std::max(max_e[sd], o.max_e[sd]);
        sides[sd].Reserve(sides[sd].size + o.sides[sd].size);
        std::memcpy(sides[sd].data + sides[sd].size, o.sides[sd].data,
                    size_t(o.sides[sd].size) * sizeof(Rec24));
        sides[sd].size += o.sides[sd].size;
        last_side_s[sd] = o.last_side_s[sd];
        o.sides[sd].size = 0;
      }
      return;
    }
    MaterializeRecs();
    o.MaterializeRecs();
    State* moved = new State();
    moved->recs.swap(o.recs);
    moved->parts.swap(o.parts);
    parts.push_back(moved);
  }

  void CollectParts(std::vector<State*>& all) {
    for (State* p : parts) {
      all.push_back(p);
      p->CollectParts(all);
      p->parts.clear();
    }
    parts.clear();
  }

  void Gather() {
    if (parts.empty())
      return;
    MaterializeRecs();
    std::vector<State*> all;
    CollectParts(all);
    size_t total = recs.size();
    for (State* p : all)
      total += p->recs.size();
    recs.reserve(total);
    for (State* p : all) {
      recs.insert(recs.end(), p->recs.begin(), p->recs.end());
      delete p;
    }
  }

  ~State() {
    for (State* p : parts)
      delete p;
  }
};

inline State* AllocState() {
  pf::Progress::AddGroup();
  return new State();
}

State*& Ptr(duckdb_aggregate_state s) {
  return *reinterpret_cast<State**>(s);
}
idx_t StateSize(duckdb_function_info) {
  return sizeof(State*);
}
void StateInit(duckdb_function_info, duckdb_aggregate_state s) {
  Ptr(s) = nullptr;
}
void StateDestroy(duckdb_aggregate_state* states, idx_t count) {
  for (idx_t i = 0; i < count; i++) {
    if (State* st = Ptr(states[i])) {
      Ptr(states[i]) = nullptr;
      delete st;
    }
  }
}
void StateCombine(duckdb_function_info,
                  duckdb_aggregate_state* source,
                  duckdb_aggregate_state* target,
                  idx_t count) {
  for (idx_t i = 0; i < count; i++) {
    State*& src = Ptr(source[i]);
    State*& dst = Ptr(target[i]);
    if (!src)
      continue;
    if (!dst) {
      dst = src;
      src = nullptr;
      continue;
    }
    dst->Merge(*src);
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
    return !validity || ((validity[r >> 6] >> (r & 63)) & 1);
  }
};

// Reads (side INT, row BIGINT, ts BIGINT, dur BIGINT [, param INT]).
void UpdateGeneric(duckdb_function_info info,
                   duckdb_data_chunk input,
                   duckdb_aggregate_state* states) {
  idx_t n = duckdb_data_chunk_get_size(input);
  pf::Progress::AddIngestedRows(n);
  idx_t ncols = duckdb_data_chunk_get_column_count(input);
  Col<int32_t> side(duckdb_data_chunk_get_vector(input, 0));
  Col<int64_t> row(duckdb_data_chunk_get_vector(input, 1));
  Col<int64_t> ts(duckdb_data_chunk_get_vector(input, 2));
  Col<int64_t> dur(duckdb_data_chunk_get_vector(input, 3));
  bool has_param = ncols > 4;
  const int32_t* param =
      has_param ? static_cast<const int32_t*>(duckdb_vector_get_data(
                      duckdb_data_chunk_get_vector(input, 4)))
                : nullptr;

  // Fast run-grouped path when no column has validity bitmasks and side in
  // {0,1}.
  if (!has_param && !ts.validity && !dur.validity && !side.validity &&
      !row.validity) {
    idx_t r = 0;
    while (r < n) {
      duckdb_aggregate_state sp = states[r];
      int32_t sd = side.data[r];
      if ((sd & ~1) != 0) {
        State*& st = Ptr(sp);
        if (!st)
          st = AllocState();
        st->MaterializeRecs();
        int64_t s = ts.data[r], d = dur.data[r];
        if (s < 0 || d < 0) {
          duckdb_aggregate_function_set_error(
              info, "pf_interval: a row's ts or dur is below zero");
          return;
        }
        int64_t end = s + d;
        st->recs.push_back({s, end, d > 0 ? end : s + 1, row.data[r], sd});
        st->total_recs++;
        r++;
        continue;
      }
      idx_t e = r + 1;
      while (e < n && states[e] == sp && side.data[e] == sd)
        e++;
      State*& st = Ptr(sp);
      if (!st)
        st = AllocState();
      if (!st->recs.empty()) {
        for (idx_t k = r; k < e; k++) {
          int64_t s = ts.data[k], d = dur.data[k];
          if (s < 0 || d < 0) {
            duckdb_aggregate_function_set_error(
                info, "pf_interval: a row's ts or dur is below zero");
            return;
          }
          int64_t end = s + d;
          st->recs.push_back({s, end, d > 0 ? end : s + 1, row.data[k], sd});
        }
        st->total_recs += (e - r);
        r = e;
        continue;
      }
      auto& vec = st->sides[sd];
      idx_t len = e - r;
      vec.Reserve(vec.size + static_cast<uint32_t>(len));
      Rec24* dst = vec.data + vec.size;
      int64_t neg = 0;
      int64_t prev_s = st->last_side_s[sd];
      bool sorted = st->side_sorted[sd];
      int64_t min_s = st->min_s[sd];
      int64_t max_e = st->max_e[sd];
      for (idx_t k = 0; k < len; k++) {
        int64_t s = ts.data[r + k];
        int64_t d = dur.data[r + k];
        neg |= s | d;
        int64_t end = s + d;
        int64_t ep = d > 0 ? end : s + 1;
        if (s < prev_s)
          sorted = false;
        prev_s = s;
        if (s < min_s)
          min_s = s;
        if (ep > max_e)
          max_e = ep;
        dst[k] = {s, end, row.data[r + k]};
      }
      if (neg < 0) {
        duckdb_aggregate_function_set_error(
            info, "pf_interval: a row's ts or dur is below zero");
        return;
      }
      vec.size += static_cast<uint32_t>(len);
      st->last_side_s[sd] = prev_s;
      st->side_sorted[sd] = sorted;
      st->min_s[sd] = min_s;
      st->max_e[sd] = max_e;
      st->total_recs += len;
      r = e;
    }
    return;
  }

  for (idx_t r = 0; r < n; r++) {
    State*& st = Ptr(states[r]);
    if (!st)
      st = AllocState();
    st->MaterializeRecs();
    if (has_param)
      st->param = param[r];
    if (!ts.Valid(r) || !dur.Valid(r) || !side.Valid(r) || !row.Valid(r))
      continue;
    int64_t s = ts.data[r], d = dur.data[r];
    if (s < 0 || (d < 0 && !(has_param && param[r] == 1 && side.data[r] == 1 &&
                             d == -1))) {
      duckdb_aggregate_function_set_error(
          info, "pf_interval: a row's ts or dur is below zero");
      return;
    }
    int64_t end = d == -1 ? std::numeric_limits<int64_t>::max() : s + d;
    int64_t ep = d > 0 ? end : (d == -1 ? end : s + 1);
    st->recs.push_back({s, end, ep, row.data[r], side.data[r]});
    st->total_recs++;
  }
}

//===----------------------------------------------------------------------===//
// Output: LIST<STRUCT(f0 BIGINT, ..., f{K-1} BIGINT [, rows BIGINT[]])>.
//===----------------------------------------------------------------------===//
struct OutBuf {
  int k = 0;                       // scalar fields per record (<= 5)
  bool has_list = false;           // trailing BIGINT[] field
  std::vector<int64_t> col[5];     // columnar scalar fields
  std::vector<uint32_t> list_off;  // per record offset into list_vals
  std::vector<int64_t> list_vals;
  std::vector<uint8_t> list_valid;  // parallel to list_vals (1 = valid)
  size_t size() const { return k ? col[0].size() : list_off.size(); }
  void Reserve(size_t n) {
    for (int f = 0; f < k; f++)
      col[f].reserve(n);
  }
  void Clear() {
    for (auto& c : col)
      c.clear();
    list_off.clear();
    list_vals.clear();
    list_valid.clear();
  }
};

inline void Emit(OutBuf& o, std::initializer_list<int64_t> v) {
  int f = 0;
  for (int64_t x : v)
    o.col[f++].push_back(x);
}
inline void BeginList(OutBuf& o) {
  o.list_off.push_back(static_cast<uint32_t>(o.list_vals.size()));
}
inline void ListPush(OutBuf& o, int64_t v, bool valid = true) {
  o.list_vals.push_back(v);
  o.list_valid.push_back(valid);
}

using Solver = void (*)(State&, OutBuf&);

// Solves every group of this finalize call into a reusable thread-local
// buffer, then copies each column into the result once.
template <int K, bool LIST, Solver SOLVE>
void FinalizeWith(duckdb_function_info,
                  duckdb_aggregate_state* source,
                  duckdb_vector result,
                  idx_t count,
                  idx_t offset) {
  thread_local OutBuf out;
  thread_local std::vector<idx_t> counts;
  out.k = K;
  out.has_list = LIST;
  out.Clear();
  counts.assign(count, 0);
  for (idx_t i = 0; i < count; i++) {
    idx_t before = out.size();
    if (State* st = Ptr(source[i])) {
      st->Gather();
      SOLVE(*st, out);
      pf::Progress::FinishGroup();
    }
    counts[i] = out.size() - before;
  }

  idx_t base0 = duckdb_list_vector_get_size(result);
  idx_t total_n = out.size();
  duckdb_list_vector_reserve(result, base0 + total_n);
  auto* entries =
      static_cast<duckdb_list_entry*>(duckdb_vector_get_data(result));
  duckdb_vector child = duckdb_list_vector_get_child(result);
  for (int f = 0; f < K; f++) {
    int64_t* dst = static_cast<int64_t*>(
        duckdb_vector_get_data(duckdb_struct_vector_get_child(child, f)));
    if (total_n)
      std::memcpy(dst + base0, out.col[f].data(), total_n * sizeof(int64_t));
  }
  if (LIST) {
    idx_t total_l = out.list_vals.size();
    duckdb_vector lv = duckdb_struct_vector_get_child(child, K);
    idx_t lbase = duckdb_list_vector_get_size(lv);
    duckdb_list_vector_reserve(lv, lbase + total_l);
    auto* lentries =
        static_cast<duckdb_list_entry*>(duckdb_vector_get_data(lv));
    duckdb_vector lchild = duckdb_list_vector_get_child(lv);
    int64_t* ld = static_cast<int64_t*>(duckdb_vector_get_data(lchild));
    if (total_l)
      std::memcpy(ld + lbase, out.list_vals.data(), total_l * sizeof(int64_t));
    for (idx_t i = 0; i < total_l; i++) {
      if (out.list_valid[i])
        continue;
      duckdb_vector_ensure_validity_writable(lchild);
      duckdb_validity_set_row_invalid(duckdb_vector_get_validity(lchild),
                                      lbase + i);
    }
    for (idx_t i = 0; i < total_n; i++) {
      uint32_t off = out.list_off[i];
      uint32_t end = i + 1 < total_n ? out.list_off[i + 1]
                                     : static_cast<uint32_t>(total_l);
      lentries[base0 + i].offset = lbase + off;
      lentries[base0 + i].length = end - off;
    }
    duckdb_list_vector_set_size(lv, lbase + total_l);
  }
  idx_t base = base0;
  for (idx_t r = 0; r < count; r++) {
    entries[offset + r].offset = base;
    entries[offset + r].length = counts[r];
    base += counts[r];
  }
  duckdb_list_vector_set_size(result, base);
}

//===----------------------------------------------------------------------===//
// 2-way overlap sweep: calls emit(a, b) for every overlapping pair, A and B
// sorted by start. Each pair is found once, when its later-starting member
// arrives (A first on ties). Skips pushing dead intervals onto active lists.
//===----------------------------------------------------------------------===//
template <typename F>
void OverlapSweep(const std::vector<Rec>& A,
                  const std::vector<Rec>& B,
                  F emit) {
  thread_local std::vector<uint32_t> act_a, act_b;
  act_a.clear();
  act_b.clear();
  size_t i = 0, j = 0;
  while (i < A.size() || j < B.size()) {
    if (j >= B.size() || (i < A.size() && A[i].s <= B[j].s)) {
      if (j >= B.size() && act_b.empty())
        break;
      const Rec& x = A[i];
      size_t w = 0;
      for (size_t k = 0; k < act_b.size(); k++) {
        const Rec& y = B[act_b[k]];
        if (y.ep > x.s) {
          act_b[w++] = act_b[k];
          emit(x, y);
        }
      }
      act_b.resize(w);
      if (j < B.size() && x.ep > B[j].s)
        act_a.push_back(static_cast<uint32_t>(i));
      i++;
    } else {
      if (i >= A.size() && act_a.empty())
        break;
      const Rec& x = B[j];
      size_t w = 0;
      for (size_t k = 0; k < act_a.size(); k++) {
        const Rec& y = A[act_a[k]];
        if (y.ep > x.s) {
          act_a[w++] = act_a[k];
          emit(y, x);
        }
      }
      act_a.resize(w);
      if (i < A.size() && x.ep > A[i].s)
        act_b.push_back(static_cast<uint32_t>(j));
      j++;
    }
  }
}

template <typename EmitPair>
void Sweep24(const Rec24* A,
             size_t na,
             const Rec24* B,
             size_t nb,
             EmitPair emit) {
  thread_local std::vector<uint32_t> act_a, act_b;
  act_a.clear();
  act_b.clear();
  size_t i = 0, j = 0;
  while (i < na || j < nb) {
    if (j >= nb || (i < na && A[i].s <= B[j].s)) {
      if (act_b.empty()) {
        if (j >= nb)
          break;
        const int64_t bs = B[j].s;
        while (i < na && A[i].s <= bs) {
          const int64_t ep = A[i].e + (A[i].e == A[i].s);
          if (ep > bs)
            act_a.push_back(static_cast<uint32_t>(i));
          i++;
        }
        continue;
      }
      const Rec24& x = A[i];
      size_t w = 0;
      for (size_t k = 0; k < act_b.size(); k++) {
        const Rec24& y = B[act_b[k]];
        const int64_t yep = y.e + (y.e == y.s);
        if (yep > x.s) {
          act_b[w++] = act_b[k];
          emit(x, y);
        }
      }
      act_b.resize(w);
      const int64_t xep = x.e + (x.e == x.s);
      if (j < nb && xep > B[j].s)
        act_a.push_back(static_cast<uint32_t>(i));
      i++;
    } else {
      if (act_a.empty()) {
        if (i >= na)
          break;
        const int64_t as = A[i].s;
        while (j < nb && B[j].s < as) {
          const int64_t ep = B[j].e + (B[j].e == B[j].s);
          if (ep > as)
            act_b.push_back(static_cast<uint32_t>(j));
          j++;
        }
        continue;
      }
      const Rec24& x = B[j];
      size_t w = 0;
      for (size_t k = 0; k < act_a.size(); k++) {
        const Rec24& y = A[act_a[k]];
        const int64_t yep = y.e + (y.e == y.s);
        if (yep > x.s) {
          act_a[w++] = act_a[k];
          emit(y, x);
        }
      }
      act_a.resize(w);
      const int64_t xep = x.e + (x.e == x.s);
      if (i < na && xep > A[i].s)
        act_b.push_back(static_cast<uint32_t>(j));
      j++;
    }
  }
}

void SplitSides(State& st, int nsides, std::vector<std::vector<Rec>>& out) {
  st.MaterializeRecs();
  out.assign(nsides, {});
  for (const Rec& r : st.recs)
    if (r.side >= 0 && r.side < nsides)
      out[r.side].push_back(r);
  for (auto& v : out)
    if (!std::is_sorted(v.begin(), v.end(), ByStart))
      std::sort(v.begin(), v.end(), ByStart);
}

//===----------------------------------------------------------------------===//
// INTERVAL INTERSECTION OF (N operands): successive narrowing, smallest
// operand first (as TP). Fast direct paths for N=2 and N=3 (no nested list).
//===----------------------------------------------------------------------===//
void SolveIntersect2(State& st, OutBuf& out) {
  if (!st.recs.empty() && st.sides[0].size == 0 && st.sides[1].size == 0) {
    // Fallback if populated via MaterializeRecs.
    std::vector<std::vector<Rec>> ops;
    SplitSides(st, 2, ops);
    if (ops[0].empty() || ops[1].empty())
      return;
    OverlapSweep(ops[0], ops[1], [&](const Rec& a, const Rec& b) {
      int64_t s = std::max(a.s, b.s);
      int64_t e = std::max(s, std::min(a.e, b.e));
      Emit(out, {s, e - s, a.row, b.row});
    });
    return;
  }
  const size_t n0 = st.sides[0].size, n1 = st.sides[1].size;
  if (n0 == 0 || n1 == 0 || st.max_e[0] <= st.min_s[1] ||
      st.max_e[1] <= st.min_s[0]) {
    return;
  }
  auto by_start24 = [](const Rec24& a, const Rec24& b) {
    return a.s != b.s ? a.s < b.s : a.e < b.e;
  };
  if (!st.side_sorted[0]) {
    std::sort(st.sides[0].data, st.sides[0].data + n0, by_start24);
    st.side_sorted[0] = true;
  }
  if (!st.side_sorted[1]) {
    std::sort(st.sides[1].data, st.sides[1].data + n1, by_start24);
    st.side_sorted[1] = true;
  }
  Sweep24(st.sides[0].data, n0, st.sides[1].data, n1,
          [&](const Rec24& a, const Rec24& b) {
            int64_t s = std::max(a.s, b.s);
            int64_t e = std::max(s, std::min(a.e, b.e));
            Emit(out, {s, e - s, a.row, b.row});
          });
}

void SolveIntersect3(State& st, OutBuf& out) {
  std::vector<std::vector<Rec>> ops;
  SplitSides(st, 3, ops);
  if (ops[0].empty() || ops[1].empty() || ops[2].empty())
    return;
  int order[3] = {0, 1, 2};
  std::stable_sort(order, order + 3,
                   [&](int a, int b) { return ops[a].size() < ops[b].size(); });
  int o0 = order[0], o1 = order[1], o2 = order[2];
  std::vector<Rec> mid;
  std::vector<std::pair<int64_t, int64_t>> mid_rows;
  OverlapSweep(ops[o0], ops[o1], [&](const Rec& a, const Rec& b) {
    int64_t s = std::max(a.s, b.s);
    int64_t e = std::max(s, std::min(a.e, b.e));
    mid.push_back(
        {s, e, e > s ? e : s + 1, static_cast<int64_t>(mid.size()), 0});
    mid_rows.push_back({a.row, b.row});
  });
  if (mid.empty())
    return;
  if (!std::is_sorted(mid.begin(), mid.end(), ByStart)) {
    std::sort(mid.begin(), mid.end(), ByStart);
  }
  OverlapSweep(mid, ops[o2], [&](const Rec& a, const Rec& b) {
    int64_t s = std::max(a.s, b.s);
    int64_t e = std::max(s, std::min(a.e, b.e));
    int64_t r[3];
    auto pr = mid_rows[static_cast<size_t>(a.row)];
    r[o0] = pr.first;
    r[o1] = pr.second;
    r[o2] = b.row;
    Emit(out, {s, e - s, r[0], r[1], r[2]});
  });
}

void SolveIntersect(State& st, OutBuf& out) {
  int n = static_cast<int>(st.param);
  if (n < 2)
    return;
  std::vector<std::vector<Rec>> ops;
  SplitSides(st, n, ops);
  std::vector<int> order(n);
  for (int i = 0; i < n; i++) {
    if (ops[i].empty())
      return;
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(),
                   [&](int a, int b) { return ops[a].size() < ops[b].size(); });
  // Running regions: Rec (bounds; row = index into rows table).
  std::vector<Rec> run, next;
  std::vector<int64_t> rows, next_rows;
  for (const Rec& r : ops[order[0]]) {
    Rec x = r;
    x.row = static_cast<int64_t>(run.size());
    run.push_back(x);
    for (int k = 0; k < n; k++)
      rows.push_back(k == order[0] ? r.row : kNone);
  }
  for (int step = 1; step < n && !run.empty(); step++) {
    int which = order[step];
    next.clear();
    next_rows.clear();
    OverlapSweep(run, ops[which], [&](const Rec& a, const Rec& b) {
      int64_t s = std::max(a.s, b.s);
      int64_t e = std::max(s, std::min(a.e, b.e));
      Rec x{s, e, e > s ? e : s + 1, static_cast<int64_t>(next.size()), 0};
      next.push_back(x);
      const int64_t* from = &rows[static_cast<size_t>(a.row) * n];
      next_rows.insert(next_rows.end(), from, from + n);
      next_rows[next_rows.size() - n + which] = b.row;
    });
    // Re-sort regions by start for the next narrowing step, remapping rows.
    std::vector<uint32_t> idx(next.size());
    for (size_t i = 0; i < idx.size(); i++)
      idx[i] = static_cast<uint32_t>(i);
    if (step + 1 < n) {
      std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
        return ByStart(next[a], next[b]);
      });
    }
    run.clear();
    rows.clear();
    for (uint32_t i : idx) {
      Rec x = next[i];
      x.row = static_cast<int64_t>(run.size());
      run.push_back(x);
      rows.insert(rows.end(), next_rows.begin() + static_cast<size_t>(i) * n,
                  next_rows.begin() + static_cast<size_t>(i + 1) * n);
    }
  }
  for (size_t i = 0; i < run.size(); i++) {
    Emit(out, {run[i].s, run[i].e - run[i].s});
    BeginList(out);
    for (int k = 0; k < n; k++)
      ListPush(out, rows[i * n + k]);
  }
}

//===----------------------------------------------------------------------===//
// Elementary-segment sweep shared by FLATTEN and UNION (TP FLATTEN rules):
// positive rows cover [s, e); segments split at every distinct start (of any
// row, points included) and end; rows with dur = 0 at a timestamp produce one
// extra zero-width segment holding those points plus the positive rows live
// at that instant (starting there included, ending there excluded). Gaps emit
// nothing. Calls seg(ts, dur, live_positive, points_or_null).
//===----------------------------------------------------------------------===//
template <typename H>
void HookSweep(std::vector<Rec>& recs, H& h) {
  if (!std::is_sorted(recs.begin(), recs.end(),
                      [](const Rec& a, const Rec& b) { return a.s < b.s; }))
    std::sort(recs.begin(), recs.end(),
              [](const Rec& a, const Rec& b) { return a.s < b.s; });
  using E = std::pair<int64_t, uint32_t>;
  auto cmp = [](const E& a, const E& b) { return a.first > b.first; };
  std::vector<E> heap;
  size_t live = 0, instants = 0;
  int64_t cursor = 0, input_ts = 0;
  auto advance = [&](int64_t time) {
    if (instants) {
      h.Seg(input_ts, 0, true);
      h.ClearInstants();
      instants = 0;
    }
    while (!heap.empty() && heap.front().first <= time) {
      int64_t end = heap.front().first;
      if (live && cursor < end)
        h.Seg(cursor, end - cursor, false);
      cursor = end;
      while (!heap.empty() && heap.front().first == end) {
        h.Remove(heap.front().second);
        live--;
        std::pop_heap(heap.begin(), heap.end(), cmp);
        heap.pop_back();
      }
    }
    if (live && cursor < time)
      h.Seg(cursor, time - cursor, false);
    cursor = time;
  };
  bool first = true;
  for (uint32_t i = 0; i < recs.size(); i++) {
    const Rec& r = recs[i];
    if (first || r.s != input_ts) {
      advance(r.s);
      input_ts = r.s;
      first = false;
    }
    if (r.e > r.s) {
      h.Add(i);
      live++;
      heap.push_back({r.e, i});
      std::push_heap(heap.begin(), heap.end(), cmp);
    } else {
      h.AddInstant(i);
      instants++;
    }
  }
  if (!first)
    advance(std::numeric_limits<int64_t>::max());
}

// Keeps the live set explicitly: seg(ts, dur, live, instants_or_null).
template <typename F>
void SegmentSweep(std::vector<Rec>& recs, F seg) {
  struct Hooks {
    F& seg;
    std::vector<uint32_t> live, pos, inst;
    void Add(uint32_t i) {
      pos[i] = static_cast<uint32_t>(live.size());
      live.push_back(i);
    }
    void Remove(uint32_t i) {
      uint32_t p = pos[i], last = live.back();
      live[p] = last;
      pos[last] = p;
      live.pop_back();
    }
    void AddInstant(uint32_t i) { inst.push_back(i); }
    void ClearInstants() { inst.clear(); }
    void Seg(int64_t ts, int64_t dur, bool with_inst) {
      seg(ts, dur, live, with_inst ? &inst : nullptr);
    }
  } h{seg, {}, std::vector<uint32_t>(recs.size()), {}};
  HookSweep(recs, h);
}

// INTERVAL FLATTEN, generic: (ts, dur, rows[]) per segment.
void SolveFlatten(State& st, OutBuf& out) {
  st.MaterializeRecs();
  auto& recs = st.recs;
  SegmentSweep(recs,
               [&](int64_t ts, int64_t dur, const std::vector<uint32_t>& live,
                   const std::vector<uint32_t>* inst) {
                 Emit(out, {ts, dur});
                 BeginList(out);
                 for (uint32_t i : live)
                   ListPush(out, recs[i].row);
                 if (inst)
                   for (uint32_t i : *inst)
                     ListPush(out, recs[i].row);
               });
}

void UpdateFlattenSum0(duckdb_function_info info,
                       duckdb_data_chunk input,
                       duckdb_aggregate_state* states) {
  idx_t n = duckdb_data_chunk_get_size(input);
  Col<int64_t> ts(duckdb_data_chunk_get_vector(input, 0));
  Col<int64_t> dur(duckdb_data_chunk_get_vector(input, 1));
  const bool all_valid = !ts.validity && !dur.validity;

  idx_t r = 0;
  while (r < n) {
    State*& st_ref = Ptr(states[r]);
    if (!st_ref)
      st_ref = AllocState();
    State* st = st_ref;
    idx_t run_end = r + 1;
    while (run_end < n && Ptr(states[run_end]) == st)
      run_end++;

    for (idx_t i = r; i < run_end; i++) {
      if (!all_valid && (!ts.Valid(i) || !dur.Valid(i)))
        continue;
      int64_t s = ts.data[i], d = dur.data[i];
      if ((s | d) < 0) {
        duckdb_aggregate_function_set_error(
            info, "pf_interval_flatten: a row's ts or dur is below zero");
        return;
      }
      if (st->spans_sorted) {
        if (s < st->last_span_s)
          st->spans_sorted = false;
        else
          st->last_span_s = s;
      }
      st->spans.push_back({s, s + d});
      st->total_recs++;
    }
    r = run_end;
  }
}

//===----------------------------------------------------------------------===//
// INTERVAL UNION OF (N operands): elementary segments where any operand
// covers, one fragment per combination of each operand's live row (kNone
// where the operand is absent); consecutive segments carrying the same
// combination coalesce. Zero-width point segments follow FLATTEN's rules.
//===----------------------------------------------------------------------===//
void SolveUnion(State& st, OutBuf& out) {
  int n = static_cast<int>(st.param);
  if (n < 1)
    return;
  st.MaterializeRecs();
  auto& recs = st.recs;
  struct VecHash {
    size_t operator()(const std::vector<int64_t>& v) const {
      size_t h = 1469598103934665603ull;
      for (int64_t x : v)
        h = (h ^ static_cast<size_t>(x)) * 1099511628211ull;
      return h;
    }
  };
  std::unordered_map<std::vector<int64_t>, int64_t, VecHash> open, still;
  int64_t open_end = std::numeric_limits<int64_t>::min();
  std::vector<std::vector<int64_t>> per(n);
  auto emit_frag = [&](const std::vector<int64_t>& t, int64_t s, int64_t e) {
    Emit(out, {s, e - s});
    BeginList(out);
    for (int64_t v : t)
      ListPush(out, v);
  };
  auto flush = [&]() {
    for (auto& kv : open)
      emit_frag(kv.first, kv.second, open_end);
    open.clear();
  };
  auto combos = [&](auto fn) {
    std::vector<int64_t> t(n, kNone);
    std::vector<size_t> ix(n, 0);
    while (true) {
      for (int k = 0; k < n; k++)
        t[k] = per[k].empty() ? kNone : per[k][ix[k]];
      fn(t);
      int k = 0;
      for (; k < n; k++) {
        if (per[k].empty())
          continue;
        if (++ix[k] < per[k].size())
          break;
        ix[k] = 0;
      }
      if (k == n)
        break;
    }
  };
  SegmentSweep(
      recs, [&](int64_t ts, int64_t dur, const std::vector<uint32_t>& live,
                const std::vector<uint32_t>* inst) {
        for (auto& v : per)
          v.clear();
        for (uint32_t i : live)
          per[recs[i].side].push_back(recs[i].row);
        if (inst)
          for (uint32_t i : *inst)
            per[recs[i].side].push_back(recs[i].row);
        for (auto& v : per)
          std::sort(v.begin(), v.end());
        if (dur == 0) {
          combos([&](const std::vector<int64_t>& t) { emit_frag(t, ts, ts); });
          return;
        }
        if (ts != open_end)
          flush();
        still.clear();
        combos([&](const std::vector<int64_t>& t) {
          auto it = open.find(t);
          still.emplace(t, it == open.end() ? ts : it->second);
          if (it != open.end())
            open.erase(it);
        });
        // Whatever remains in `open` ended at this boundary.
        for (auto& kv : open)
          emit_frag(kv.first, kv.second, open_end);
        open.swap(still);
        open_end = ts + dur;
      });
  flush();
}

//===----------------------------------------------------------------------===//
// INTERVAL SPLIT: tile each side-0 row at side-1 boundaries; (ts, dur, r0, r1)
// with r1 = kNone where no operand row covers the piece.
//===----------------------------------------------------------------------===//
void SolveSplit(State& st, OutBuf& out) {
  std::vector<std::vector<Rec>> sides;
  SplitSides(st, 2, sides);
  auto& A = sides[0];
  auto& B = sides[1];
  // Overlapping operand rows per A row (by index in A).
  std::vector<std::pair<uint32_t, uint32_t>> pairs;
  {
    std::vector<Rec> Ai = A;
    for (size_t i = 0; i < Ai.size(); i++)
      Ai[i].row = static_cast<int64_t>(i);
    std::vector<Rec> Bi = B;
    for (size_t i = 0; i < Bi.size(); i++)
      Bi[i].row = static_cast<int64_t>(i);
    OverlapSweep(Ai, Bi, [&](const Rec& a, const Rec& b) {
      pairs.push_back(
          {static_cast<uint32_t>(a.row), static_cast<uint32_t>(b.row)});
    });
  }
  std::sort(pairs.begin(), pairs.end());
  std::vector<int64_t> cuts;
  std::vector<const Rec*> ov;
  size_t p = 0;
  for (size_t i = 0; i < A.size(); i++) {
    const Rec& a = A[i];
    ov.clear();
    while (p < pairs.size() && pairs[p].first == i)
      ov.push_back(&B[pairs[p++].second]);
    if (ov.empty()) {
      Emit(out, {a.s, a.e - a.s, a.row, kNone});
      continue;
    }
    if (a.e == a.s) {  // a point: one piece per covering operand row
      for (const Rec* b : ov)
        Emit(out, {a.s, 0, a.row, b->row});
      continue;
    }
    cuts.clear();
    cuts.push_back(a.s);
    cuts.push_back(a.e);
    for (const Rec* b : ov) {
      if (b->s > a.s && b->s < a.e)
        cuts.push_back(b->s);
      if (b->e > a.s && b->e < a.e)
        cuts.push_back(b->e);
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    for (size_t c = 0; c + 1 < cuts.size(); c++) {
      int64_t s = cuts[c], e = cuts[c + 1];
      // Zero-width piece for operand points at s (attached with positive
      // operand rows live at s), as FLATTEN does.
      bool has_point = false;
      for (const Rec* b : ov)
        has_point |= (b->e == b->s && b->s == s);
      if (has_point) {
        for (const Rec* b : ov)
          if ((b->e == b->s && b->s == s) ||
              (b->e > b->s && b->s <= s && s < b->e))
            Emit(out, {s, 0, a.row, b->row});
      }
      bool any = false;
      for (const Rec* b : ov)
        if (b->e > b->s && b->s <= s && e <= b->e) {
          Emit(out, {s, e - s, a.row, b->row});
          any = true;
        }
      if (!any)
        Emit(out, {s, e - s, a.row, kNone});
    }
  }
}

//===----------------------------------------------------------------------===//
// Merged coverage of a sorted set: disjoint positive intervals (overlapping
// and abutting rows coalesce) plus isolated points not inside them.
//===----------------------------------------------------------------------===//
struct Coverage {
  std::vector<int64_t> cs, ce;  // positive, disjoint, sorted
  std::vector<int64_t> pts;     // sorted points outside [cs, ce)
  void Build(const std::vector<Rec>& sorted) {
    std::vector<int64_t> raw_pts;
    for (const Rec& r : sorted) {
      if (r.e == r.s) {
        raw_pts.push_back(r.s);
        continue;
      }
      if (!cs.empty() && r.s <= ce.back()) {
        ce.back() = std::max(ce.back(), r.e);
      } else {
        cs.push_back(r.s);
        ce.push_back(r.e);
      }
    }
    for (int64_t p : raw_pts)
      if (!CoversInstantPositive(p) && (pts.empty() || pts.back() != p))
        pts.push_back(p);
  }
  // Index of the first coverage interval with ce > t.
  size_t FirstEndingAfter(int64_t t) const {
    return static_cast<size_t>(std::upper_bound(ce.begin(), ce.end(), t) -
                               ce.begin());
  }
  bool CoversInstantPositive(int64_t t) const {
    size_t k = FirstEndingAfter(t);
    return k < cs.size() && cs[k] <= t;
  }
  bool CoversInstant(int64_t t) const {
    return CoversInstantPositive(t) ||
           std::binary_search(pts.begin(), pts.end(), t);
  }
};

//===----------------------------------------------------------------------===//
// INTERVAL SUBTRACT: side-0 rows minus side-1 coverage; (ts, dur, r0).
//===----------------------------------------------------------------------===//
void SolveSubtract(State& st, OutBuf& out) {
  std::vector<std::vector<Rec>> sides;
  SplitSides(st, 2, sides);
  Coverage cov;
  cov.Build(sides[1]);
  for (const Rec& a : sides[0]) {
    if (a.e == a.s) {
      if (!cov.CoversInstant(a.s))
        Emit(out, {a.s, 0, a.row});
      continue;
    }
    int64_t cur = a.s;
    for (size_t k = cov.FirstEndingAfter(a.s);
         k < cov.cs.size() && cov.cs[k] < a.e; k++) {
      if (cov.cs[k] > cur)
        Emit(out, {cur, cov.cs[k] - cur, a.row});
      cur = std::max(cur, cov.ce[k]);
      if (cur >= a.e)
        break;
    }
    if (cur < a.e)
      Emit(out, {cur, a.e - cur, a.row});
  }
}

//===----------------------------------------------------------------------===//
// Relationships (JOIN against rows, KEEP/DROP against merged coverage).
//===----------------------------------------------------------------------===//
enum Rel : int64_t {
  kOverlapping = 0,
  kCoveringBegin = 1,
  kCoveringEnd = 2,
  kCoveringBounds = 3,
  kWithinBounds = 4,
};

// Does operand row b stand in `rel` to my row a?
inline bool RelHolds(int64_t rel, const Rec& a, const Rec& b) {
  bool a_pt = a.e == a.s, b_pt = b.e == b.s;
  auto covers_instant = [&](int64_t t) {
    return b_pt ? b.s == t : (b.s <= t && t < b.e);
  };
  switch (rel) {
    case kOverlapping:
      return Overlaps(a, b);
    case kCoveringBegin:
      return covers_instant(a.s);
    case kCoveringEnd:
      return covers_instant(a.e);
    case kCoveringBounds:
      if (a_pt)
        return covers_instant(a.s);
      return !b_pt && b.s <= a.s && a.e <= b.e;
    case kWithinBounds:
      if (a_pt)
        return b_pt && b.s == a.s;
      if (b_pt)
        return a.s <= b.s && b.s < a.e;
      return a.s <= b.s && b.e <= a.e;
  }
  return false;
}

// INTERVAL JOIN: (r0, r1) per match; r1 = kNone for unmatched rows when the
// relationship code has bit 8 set (LEFT).
void SolveJoin(State& st, OutBuf& out) {
  int64_t rel = st.param & 0xff;
  bool left = (st.param & 0x100) != 0;
  std::vector<std::vector<Rec>> sides;
  SplitSides(st, 2, sides);
  auto& A = sides[0];
  auto& B = sides[1];
  std::vector<uint8_t> matched(left ? A.size() : 0, 0);
  // Probes: for COVERING BEGIN/END, a's anchor instant as a point; otherwise
  // a itself. Every relationship implies the probe overlaps b.
  std::vector<Rec> probe(A.size());
  for (size_t i = 0; i < A.size(); i++) {
    Rec p = A[i];
    if (rel == kCoveringBegin || rel == kCoveringEnd) {
      int64_t t = rel == kCoveringBegin ? A[i].s : A[i].e;
      p = {t, t, t + 1, 0, 0};
    }
    p.row = static_cast<int64_t>(i);
    probe[i] = p;
  }
  if (rel == kCoveringEnd)
    std::sort(probe.begin(), probe.end(), ByStart);
  OverlapSweep(probe, B, [&](const Rec& p, const Rec& b) {
    const Rec& a = A[static_cast<size_t>(p.row)];
    if (!RelHolds(rel, a, b))
      return;
    Emit(out, {a.row, b.row});
    if (left)
      matched[static_cast<size_t>(p.row)] = 1;
  });
  if (left)
    for (size_t i = 0; i < A.size(); i++)
      if (!matched[i])
        Emit(out, {A[i].row, kNone});
}

// INTERVAL FIND: param encodes:
//   bit 0: edge_end (0 = starting [b.s], 1 = ending [b.e])
//   bit 1: dir_before (0 = after [y >= x], 1 = before [y <= x])
//   bits 2..3: anchor (0 = begin [a.s], 1 = end [a.e], 2 = bounds)
//   bit 8: left_join (emit (a.row, kNone) when unmatched)
void SolveFind(State& st, OutBuf& out) {
  bool edge_end = (st.param & 1) != 0;
  bool dir_before = (st.param & 2) != 0;
  int anchor = (st.param >> 2) & 3;
  bool left = (st.param & 0x100) != 0;

  std::vector<std::vector<Rec>> sides;
  SplitSides(st, 2, sides);
  auto& A = sides[0];
  auto& B = sides[1];
  if (A.empty())
    return;
  out.Reserve(A.size());
  if (B.empty()) {
    if (left) {
      for (const Rec& a : A)
        Emit(out, {a.row, kNone});
    }
    return;
  }

  auto y_of = [&](const Rec& b) { return edge_end ? b.e : b.s; };
  auto cmp_b = [&](const Rec& p, const Rec& q) {
    int64_t yp = y_of(p), yq = y_of(q);
    if (yp != yq)
      return yp < yq;
    if (dir_before) {
      if (p.s != q.s)
        return p.s > q.s;
      return (p.e - p.s) > (q.e - q.s);
    }
    if (p.s != q.s)
      return p.s < q.s;
    return (p.e - p.s) < (q.e - q.s);
  };
  if (!std::is_sorted(B.begin(), B.end(), cmp_b))
    std::sort(B.begin(), B.end(), cmp_b);

  const Rec* b_beg = B.data();
  const Rec* b_end = B.data() + B.size();
  for (const Rec& a : A) {
    int64_t x = (anchor == 0 || (anchor == 2 && dir_before)) ? a.s : a.e;
    const Rec* hit = nullptr;
    if (!dir_before) {
      const Rec* it = std::lower_bound(
          b_beg, b_end, x,
          [&](const Rec& b, int64_t v) { return y_of(b) < v; });
      if (it != b_end)
        hit = it;
    } else {
      const Rec* it = std::upper_bound(
          b_beg, b_end, x,
          [&](int64_t v, const Rec& b) { return v < y_of(b); });
      if (it != b_beg)
        hit = it - 1;
    }
    if (hit)
      Emit(out, {a.row, hit->row});
    else if (left)
      Emit(out, {a.row, kNone});
  }
}

void UpdateQuantize(duckdb_function_info info,
                    duckdb_data_chunk input,
                    duckdb_aggregate_state* states) {
  idx_t n = duckdb_data_chunk_get_size(input);
  Col<int64_t> row(duckdb_data_chunk_get_vector(input, 0));
  Col<int64_t> ts(duckdb_data_chunk_get_vector(input, 1));
  Col<int64_t> dur(duckdb_data_chunk_get_vector(input, 2));
  Col<int64_t> orig(duckdb_data_chunk_get_vector(input, 3));
  Col<int64_t> wid(duckdb_data_chunk_get_vector(input, 4));
  for (idx_t r = 0; r < n; r++) {
    State*& st = Ptr(states[r]);
    if (!st)
      st = AllocState();
    if (!ts.Valid(r) || !dur.Valid(r) || !wid.Valid(r))
      continue;
    int64_t s = ts.data[r], d = dur.data[r], w = wid.data[r];
    if (__builtin_expect(d < 0, 0)) {
      duckdb_aggregate_function_set_error(
          info, "pf_interval_quantize: dur below zero");
      return;
    }
    int64_t o = orig.Valid(r) ? orig.data[r] : 0;
    st->param = w;
    st->recs.push_back({s, d, o, row.data[r], 0});
    st->total_recs++;
  }
}

// Streaming vectorized scalar kernel for INTERVAL QUANTIZE:
// pf_interval_quantize_segs(ts, dur, origin, width)
//   -> LIST<STRUCT(ts BIGINT, dur BIGINT, bucket BIGINT)>
// Processes each 2048-row vector chunk directly in L1 cache across worker
// threads without allocating State or running SQL scalar arithmetic loops.
void QuantizeSegsScalar(duckdb_function_info info,
                        duckdb_data_chunk input,
                        duckdb_vector output) {
  idx_t n = duckdb_data_chunk_get_size(input);
  Col<int64_t> ts(duckdb_data_chunk_get_vector(input, 0));
  Col<int64_t> dur(duckdb_data_chunk_get_vector(input, 1));
  Col<int64_t> orig(duckdb_data_chunk_get_vector(input, 2));
  Col<int64_t> wid(duckdb_data_chunk_get_vector(input, 3));

  idx_t total = 0;
  for (idx_t r = 0; r < n; r++) {
    if (!ts.Valid(r) || !dur.Valid(r) || !wid.Valid(r))
      continue;
    int64_t s = ts.data[r], d = dur.data[r], w = wid.data[r];
    if (__builtin_expect(d < 0, 0)) {
      duckdb_scalar_function_set_error(info,
                                       "pf_interval_quantize: dur below zero");
      return;
    }
    int64_t o = orig.Valid(r) ? orig.data[r] : 0;
    int64_t d0 = s - o;
    int64_t k0 = d0 >= 0 ? (d0 / w) : ((d0 + 1) / w - 1);
    int64_t k1 = k0;
    if (d > 0) {
      int64_t d1 = s + d - 1 - o;
      k1 = d1 >= 0 ? (d1 / w) : ((d1 + 1) / w - 1);
    }
    total += static_cast<idx_t>(k1 - k0 + 1);
  }

  duckdb_list_vector_reserve(output, total);
  duckdb_list_vector_set_size(output, total);
  auto* entries =
      static_cast<duckdb_list_entry*>(duckdb_vector_get_data(output));
  duckdb_vector elem = duckdb_list_vector_get_child(output);
  auto* out_ts = static_cast<int64_t*>(
      duckdb_vector_get_data(duckdb_struct_vector_get_child(elem, 0)));
  auto* out_dur = static_cast<int64_t*>(
      duckdb_vector_get_data(duckdb_struct_vector_get_child(elem, 1)));
  auto* out_bkt = static_cast<int64_t*>(
      duckdb_vector_get_data(duckdb_struct_vector_get_child(elem, 2)));

  idx_t p = 0;
  for (idx_t r = 0; r < n; r++) {
    if (!ts.Valid(r) || !dur.Valid(r) || !wid.Valid(r)) {
      entries[r] = {p, 0};
      continue;
    }
    int64_t s = ts.data[r], d = dur.data[r], w = wid.data[r];
    int64_t o = orig.Valid(r) ? orig.data[r] : 0;
    int64_t d0 = s - o;
    int64_t k0 = d0 >= 0 ? (d0 / w) : ((d0 + 1) / w - 1);
    if (d == 0) {
      int64_t b = o + k0 * w;
      out_ts[p] = s;
      out_dur[p] = 0;
      out_bkt[p] = b;
      entries[r] = {p, 1};
      p++;
      continue;
    }
    int64_t d1 = s + d - 1 - o;
    int64_t k1 = d1 >= 0 ? (d1 / w) : ((d1 + 1) / w - 1);
    int64_t cnt = k1 - k0 + 1;
    entries[r] = {p, static_cast<idx_t>(cnt)};
    int64_t b = o + k0 * w;
    if (cnt == 1) {
      out_ts[p] = s;
      out_dur[p] = d;
      out_bkt[p] = b;
      p++;
    } else {
      int64_t first_ts = std::max(s, b);
      out_ts[p] = first_ts;
      out_dur[p] = (b + w) - first_ts;
      out_bkt[p] = b;
      b += w;
      p++;
      for (int64_t k = k0 + 1; k < k1; k++) {
        out_ts[p] = b;
        out_dur[p] = w;
        out_bkt[p] = b;
        b += w;
        p++;
      }
      out_ts[p] = b;
      out_dur[p] = (s + d) - b;
      out_bkt[p] = b;
      p++;
    }
  }
}

// Fast single-pass C++ kernel for INTERVAL MERGE CONSECUTIVE:
// pf_interval_merge_consecutive_count_agg(row, ts, dur, khash, 1)
//   -> LIST<STRUCT(ts BIGINT, dur BIGINT, r0 BIGINT, cnt BIGINT)>
void SolveMergeConsecutiveCount(State& st, OutBuf& out) {
  auto& r = st.recs;
  const size_t n = r.size();
  if (n == 0)
    return;
  if (!std::is_sorted(r.begin(), r.end(), ByStart))
    std::sort(r.begin(), r.end(), ByStart);
  out.Reserve(n / 2 + 1);
  size_t i = 0;
  while (i < n) {
    int64_t s = r[i].s, e = r[i].s + r[i].e, k = r[i].ep;
    size_t j = i + 1;
    while (j < n && r[j].s <= (r[j - 1].s + r[j - 1].e) && r[j].ep == k) {
      e = std::max(e, r[j].s + r[j].e);
      j++;
    }
    Emit(out, {s, e - s, r[i].row, static_cast<int64_t>(j - i)});
    i = j;
  }
}

// INTERVAL KEEP/DROP IF: side-0 rows whose relationship to the side-1 merged
// coverage holds; (r0).
void SolveKeep(State& st, OutBuf& out) {
  int64_t rel = st.param;
  std::vector<std::vector<Rec>> sides;
  SplitSides(st, 2, sides);
  Coverage cov;
  cov.Build(sides[1]);
  for (const Rec& a : sides[0]) {
    bool a_pt = a.e == a.s;
    bool ok = false;
    switch (rel) {
      case kOverlapping:
        if (a_pt) {
          ok = cov.CoversInstant(a.s);
        } else {
          size_t k = cov.FirstEndingAfter(a.s);
          ok = (k < cov.cs.size() && cov.cs[k] < a.e);
          if (!ok) {
            auto it = std::lower_bound(cov.pts.begin(), cov.pts.end(), a.s);
            ok = it != cov.pts.end() && *it < a.e;
          }
        }
        break;
      case kCoveringBegin:
        ok = cov.CoversInstant(a.s);
        break;
      case kCoveringEnd:
        ok = cov.CoversInstant(a.e);
        break;
      case kCoveringBounds:
        if (a_pt) {
          ok = cov.CoversInstant(a.s);
        } else {
          size_t k = cov.FirstEndingAfter(a.s);
          ok = k < cov.cs.size() && cov.cs[k] <= a.s && a.e <= cov.ce[k];
        }
        break;
      case kWithinBounds:
        if (a_pt) {
          ok = std::binary_search(cov.pts.begin(), cov.pts.end(), a.s);
        } else {
          auto it = std::lower_bound(cov.cs.begin(), cov.cs.end(), a.s);
          ok = it != cov.cs.end() &&
               cov.ce[static_cast<size_t>(it - cov.cs.begin())] <= a.e;
          if (!ok) {
            auto pt = std::lower_bound(cov.pts.begin(), cov.pts.end(), a.s);
            ok = pt != cov.pts.end() && *pt < a.e;
          }
        }
        break;
    }
    if (ok)
      Emit(out, {a.row});
  }
}

//===----------------------------------------------------------------------===//
// INTERVAL MERGE OVERLAPPING: coalesce overlapping and abutting rows (points
// merge into an interval covering or abutting them); (ts, dur, rows[]).
//===----------------------------------------------------------------------===//
void SolveMerge(State& st, OutBuf& out) {
  st.MaterializeRecs();
  auto& r = st.recs;
  if (!std::is_sorted(r.begin(), r.end(), ByStart))
    std::sort(r.begin(), r.end(), ByStart);
  size_t i = 0;
  while (i < r.size()) {
    int64_t s = r[i].s, e = r[i].e;
    size_t j = i + 1;
    while (j < r.size() && r[j].s <= e) {
      e = std::max(e, r[j].e);
      j++;
    }
    Emit(out, {s, e - s});
    BeginList(out);
    for (size_t k = i; k < j; k++)
      ListPush(out, r[k].row);
    i = j;
  }
}

void SolveMergeCount(State& st, OutBuf& out) {
  const size_t n = st.spans.size;
  if (n == 0)
    return;
  Span16* r = st.spans.data;
  if (!st.spans_sorted) {
    std::sort(r, r + n,
              [](const Span16& a, const Span16& b) { return a.s < b.s; });
    st.spans_sorted = true;
  }
  out.Reserve(n);
  size_t i = 0;
  while (i < n) {
    int64_t s = r[i].s, e = r[i].e;
    size_t j = i + 1;
    while (j < n && r[j].s <= e) {
      e = std::max(e, r[j].e);
      j++;
    }
    Emit(out, {s, e - s, static_cast<int64_t>(j - i)});
    i = j;
  }
}

//===----------------------------------------------------------------------===//
// Registration.
//===----------------------------------------------------------------------===//
duckdb_logical_type BigInt() {
  return duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
}

duckdb_logical_type OutType(const std::vector<const char*>& scalars,
                            const char* list_name) {
  std::vector<duckdb_logical_type> types;
  std::vector<const char*> names = scalars;
  for (size_t i = 0; i < scalars.size(); i++)
    types.push_back(BigInt());
  if (list_name) {
    duckdb_logical_type b = BigInt();
    types.push_back(duckdb_create_list_type(b));
    duckdb_destroy_logical_type(&b);
    names.push_back(list_name);
  }
  duckdb_logical_type st = duckdb_create_struct_type(
      types.data(), names.data(), static_cast<idx_t>(types.size()));
  for (auto& t : types)
    duckdb_destroy_logical_type(&t);
  duckdb_logical_type list = duckdb_create_list_type(st);
  duckdb_destroy_logical_type(&st);
  return list;
}

bool Register(duckdb_connection conn,
              const char* name,
              const std::vector<duckdb_logical_type>& params,
              duckdb_logical_type ret,
              duckdb_aggregate_update_t update,
              duckdb_aggregate_finalize_t finalize) {
  duckdb_aggregate_function f = duckdb_create_aggregate_function();
  duckdb_aggregate_function_set_name(f, name);
  for (duckdb_logical_type t : params) {
    duckdb_aggregate_function_add_parameter(f, t);
    duckdb_logical_type tmp = t;
    duckdb_destroy_logical_type(&tmp);
  }
  duckdb_aggregate_function_set_return_type(f, ret);
  duckdb_aggregate_function_set_functions(f, StateSize, StateInit, update,
                                          StateCombine, finalize);
  duckdb_aggregate_function_set_destructor(f, StateDestroy);
  duckdb_aggregate_function_set_special_handling(f);
  bool ok = duckdb_register_aggregate_function(conn, f) == DuckDBSuccess;
  duckdb_destroy_aggregate_function(&f);
  duckdb_destroy_logical_type(&ret);
  return ok;
}

duckdb_logical_type T(duckdb_type t) {
  return duckdb_create_logical_type(t);
}

std::vector<duckdb_logical_type> Generic(bool with_param) {
  std::vector<duckdb_logical_type> v = {
      T(DUCKDB_TYPE_INTEGER), T(DUCKDB_TYPE_BIGINT), T(DUCKDB_TYPE_BIGINT),
      T(DUCKDB_TYPE_BIGINT)};
  if (with_param)
    v.push_back(T(DUCKDB_TYPE_INTEGER));
  return v;
}

void UpdateFromEvents(duckdb_function_info,
                      duckdb_data_chunk input,
                      duckdb_aggregate_state* states) {
  idx_t n = duckdb_data_chunk_get_size(input);
  Col<int32_t> side(duckdb_data_chunk_get_vector(input, 0));
  Col<int64_t> row(duckdb_data_chunk_get_vector(input, 1));
  Col<int64_t> ts(duckdb_data_chunk_get_vector(input, 2));
  Col<int64_t> close_at(duckdb_data_chunk_get_vector(input, 3));
  const int32_t* param = static_cast<const int32_t*>(
      duckdb_vector_get_data(duckdb_data_chunk_get_vector(input, 4)));
  for (idx_t r = 0; r < n; r++) {
    State*& st = Ptr(states[r]);
    if (!st)
      st = new State();
    st->param = param[r];
    if (!ts.Valid(r) || !side.Valid(r))
      continue;
    int32_t sd = side.data[r];
    if (sd == 0 && !row.Valid(r))
      continue;
    int64_t cl = close_at.Valid(r) ? close_at.data[r] : 0;
    st->recs.push_back({ts.data[r], cl, 0, sd == 0 ? row.data[r] : 0, sd});
    st->total_recs++;
  }
}

void SolveFromEvents(State& st, OutBuf& out) {
  auto& recs = st.recs;
  if (recs.empty())
    return;
  auto by_ev = [](const Rec& a, const Rec& b) {
    return a.s != b.s ? a.s < b.s : a.row < b.row;
  };
  bool has_stops = false;
  for (const Rec& r : recs) {
    if (r.side != 0) {
      has_stops = true;
      break;
    }
  }
  if (!has_stops) {
    if (!std::is_sorted(recs.begin(), recs.end(), by_ev))
      std::stable_sort(recs.begin(), recs.end(), by_ev);
    size_t n = recs.size();
    out.Reserve(n);
    for (size_t i = 0; i + 1 < n; i++)
      Emit(out, {recs[i].row, recs[i].s, recs[i + 1].s - recs[i].s});
    if (st.param == 1)
      Emit(out, {recs[n - 1].row, recs[n - 1].s, -1});
    else if (st.param == 2)
      Emit(out,
           {recs[n - 1].row, recs[n - 1].s, recs[n - 1].e - recs[n - 1].s});
    return;
  }
  std::vector<Rec> ev;
  std::vector<int64_t> stops;
  ev.reserve(recs.size());
  for (const Rec& r : recs) {
    if (r.side == 0)
      ev.push_back(r);
    else
      stops.push_back(r.s);
  }
  if (ev.empty())
    return;
  if (!std::is_sorted(ev.begin(), ev.end(), by_ev))
    std::stable_sort(ev.begin(), ev.end(), by_ev);
  if (!std::is_sorted(stops.begin(), stops.end()))
    std::sort(stops.begin(), stops.end());
  out.Reserve(ev.size());
  size_t sc = 0, n = ev.size();
  for (size_t i = 0; i < n; i++) {
    while (sc < stops.size() && stops[sc] < ev[i].s)
      sc++;
    bool he = i + 1 < n, hs = sc < stops.size();
    if (he || hs) {
      int64_t end_ts = (he && hs) ? std::min(ev[i + 1].s, stops[sc])
                                  : (he ? ev[i + 1].s : stops[sc]);
      Emit(out, {ev[i].row, ev[i].s, end_ts - ev[i].s});
    } else if (st.param == 1) {
      Emit(out, {ev[i].row, ev[i].s, -1});
    } else if (st.param == 2) {
      Emit(out, {ev[i].row, ev[i].s, ev[i].e - ev[i].s});
    }
  }
}

//===----------------------------------------------------------------------===//
// Partition kernels for INTERVAL FLATTEN and INTERVAL INTERSECTION.
//
// One aggregate state per trace partition holds every operand row of the
// trace as columns (lane, ts, end, id | values). Finalize renumbers the lanes
// densely, buckets rows per lane with a stable counting sort (TP tables are
// ts-ordered, so lanes come out presorted and the sort check is O(n)), and
// sweeps each lane once.
//===----------------------------------------------------------------------===//
namespace ivl {

using pf::Blocks;
using pf::Buf;
using pf::In;
using pf::ListOut;

constexpr int kMaxSides = 4;
constexpr int kMaxSums = 8;
constexpr const char* kNegErr = "pf_interval: a row's ts or dur is below zero";

// Block columns of one operand's rows.
enum : int { kLane = 0, kTs = 1, kEnd = 2, kAux = 3 };  // kAux: id | values

// Per-partition state: one row store per operand. A thread's rows form one
// part; Gather() splices the parts in ts order, so ts-ordered input stays
// ordered under parallel scans.
struct State {
  Blocks side[kMaxSides];
  std::vector<std::unique_ptr<State>> parts;
  int nsides = 1;
  int nv = 0;  // flatten: number of SUM columns
  bool err = false;

  void Absorb(State&& o) {
    err |= o.err;
    parts.push_back(std::make_unique<State>(std::move(o)));
  }
  int64_t FirstTs() const {
    for (int s = 0; s < nsides; s++)
      if (side[s].size())
        return side[s].col(0, kTs)[0];
    return INT64_MAX;
  }
  void Gather() {
    if (parts.empty())
      return;
    std::vector<std::unique_ptr<State>> all;
    all.swap(parts);
    for (size_t i = 0; i < all.size(); i++) {
      for (auto& p : all[i]->parts)
        all.push_back(std::move(p));
      all[i]->parts.clear();
    }
    auto self = std::make_unique<State>(std::move(*this));
    all.push_back(std::move(self));
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
      return a->FirstTs() < b->FirstTs();
    });
    for (auto& p : all) {
      nsides = std::max(nsides, p->nsides);
      nv = std::max(nv, p->nv);
      err |= p->err;
    }
    for (auto& p : all)
      for (int s = 0; s < nsides; s++) {
        side[s].SetWidth(p->side[s].width());
        side[s].Splice(std::move(p->side[s]));
      }
  }
};
using Ops = pf::StateOps<State>;

// Appends rows [r, e) of (lane, ts, dur, aux...) to `rows`; rows with a NULL
// ts or dur are skipped, a NULL lane / id becomes kNull. With nv > 0 the aux
// columns are SUM inputs followed by a column of their NULL bits. Returns
// false on ts or dur < 0.
bool AppendRun(Blocks& rows,
               const In<int64_t>& lane,
               const In<int64_t>& ts,
               const In<int64_t>& dur,
               const In<int64_t>* aux,
               int naux,
               bool null_bits,
               idx_t r,
               idx_t e) {
  bool clean = ts.AllValid(r, e) && dur.AllValid(r, e);
  int64_t neg = 0;
  while (r < e) {
    size_t at, fit;
    int64_t* d = rows.Room(e - r, &at, &fit);
    constexpr size_t R = Blocks::kRows;
    size_t k = 0;
    if (clean) {
      std::memcpy(d + kLane * R + at, lane.data + r, fit * 8);
      std::memcpy(d + kTs * R + at, ts.data + r, fit * 8);
      int64_t* en = d + kEnd * R + at;
      for (size_t i = 0; i < fit; i++) {
        int64_t s = ts.data[r + i], du = dur.data[r + i];
        neg |= s | du;
        en[i] = s + du;
      }
      if (lane.valid)
        for (size_t i = 0; i < fit; i++)
          if (!lane.Valid(r + i))
            d[kLane * R + at + i] = ListOut::kNull;
      for (int c = 0; c < naux; c++) {
        int64_t* o = d + (kAux + c) * R + at;
        std::memcpy(o, aux[c].data + r, fit * 8);
        if (aux[c].valid && !null_bits)
          for (size_t i = 0; i < fit; i++)
            if (!aux[c].Valid(r + i))
              o[i] = ListOut::kNull;
      }
      if (null_bits) {
        int64_t* nb = d + (kAux + naux) * R + at;
        for (size_t i = 0; i < fit; i++) {
          int64_t bits = 0;
          for (int c = 0; c < naux; c++)
            bits |= int64_t{!aux[c].Valid(r + i)} << c;
          nb[i] = bits;
        }
      }
      k = fit;
      r += fit;
    } else {
      for (; r < e && k < fit; r++) {
        if (!ts.Valid(r) || !dur.Valid(r))
          continue;
        int64_t s = ts.data[r], du = dur.data[r];
        neg |= s | du;
        size_t o = at + k++;
        d[kLane * R + o] = lane.Valid(r) ? lane.data[r] : ListOut::kNull;
        d[kTs * R + o] = s;
        d[kEnd * R + o] = s + du;
        int64_t bits = 0;
        for (int c = 0; c < naux; c++) {
          bool ok = aux[c].Valid(r);
          d[(kAux + c) * R + o] =
              ok ? aux[c].data[r] : (null_bits ? 0 : ListOut::kNull);
          bits |= int64_t{!ok} << c;
        }
        if (null_bits)
          d[(kAux + naux) * R + o] = bits;
      }
    }
    rows.Commit(k);
  }
  return neg >= 0;
}

// Sweeps. Each lane is swept by a small state machine fed the lane's rows in
// ts order. TP tables are ts-ordered, so the common path streams every
// operand once in its stored order and routes each row to its lane's sweeper
// (no bucketing, no copies); input that is not ts-ordered is first bucketed
// per lane and sorted.

// Lane renumbering plus a reusable pool of per-lane sweepers.
template <typename L>
class Lanes {
 public:
  L& Get(int64_t lane) {
    uint32_t id = ids_.Intern(lane);
    if (id == used_) {
      if (id == pool_.size())
        pool_.emplace_back();
      pool_[id].Reset(lane);
      used_++;
    }
    return pool_[id];
  }
  size_t size() const { return used_; }
  L& at(size_t i) { return pool_[i]; }
  void Reset() {
    ids_.Reset();
    used_ = 0;
  }

 private:
  pf::DenseIds ids_;
  std::vector<L> pool_;
  size_t used_ = 0;
};

template <typename L>
__attribute__((noinline)) Lanes<L>& TlsLanes() {
  static thread_local Lanes<L> lanes;
  return lanes;
}

// Feeds every row of every operand to fn(side, block, row) in ts order.
// Operands are stored as blocks in arrival order; consecutive blocks that
// continue each other's ts order form a sorted run (one run per operand for
// a single-threaded scan of a TP table, a few per thread under parallel
// scans), and the runs are merged. Ties go to the lower operand. Input with
// an unsorted block falls back to a stable sort of all rows.
template <typename F>
void ForEachInTsOrder(const Blocks* const* sides, int nsides, F&& fn) {
  struct Run {
    int side;
    size_t b, end_b;  // blocks [b, end_b)
    uint32_t j;
    int64_t ts;  // ts of the current row
  };
  static thread_local std::vector<Run> runs;
  runs.clear();
  bool sorted = true;
  for (int s = 0; s < nsides && sorted; s++) {
    const Blocks& rw = *sides[s];
    int64_t prev = INT64_MAX;
    for (size_t b = 0; b < rw.num_blocks(); b++) {
      const int64_t* ts = rw.col(b, kTs);
      uint32_t n = rw.block_rows(b);
      bool ok = true;
      for (uint32_t j = 1; j < n; j++)
        ok &= ts[j] >= ts[j - 1];
      if (!ok) {
        sorted = false;
        break;
      }
      if (ts[0] < prev)
        runs.push_back({s, b, b + 1, 0, ts[0]});
      else
        runs.back().end_b = b + 1;
      prev = ts[n - 1];
    }
  }
  if (!sorted) {
    struct Ent {
      int64_t ts;
      int side;
      uint64_t h;
    };
    static thread_local Buf<Ent> order;
    order.Clear();
    for (int s = 0; s < nsides; s++) {
      const Blocks& rw = *sides[s];
      for (size_t b = 0; b < rw.num_blocks(); b++)
        for (uint32_t j = 0; j < rw.block_rows(b); j++)
          order.Push({rw.col(b, kTs)[j], s, b << Blocks::kShift | j});
    }
    std::stable_sort(order.data(), order.data() + order.size(),
                     [](const Ent& a, const Ent& b) {
                       return a.ts != b.ts ? a.ts < b.ts : a.side < b.side;
                     });
    for (size_t i = 0; i < order.size(); i++)
      fn(order[i].side, order[i].h >> Blocks::kShift,
         static_cast<uint32_t>(order[i].h & (Blocks::kRows - 1)));
    return;
  }
  if (runs.size() == 1) {
    const Run& r = runs[0];
    const Blocks& rw = *sides[r.side];
    for (size_t b = r.b; b < r.end_b; b++)
      for (uint32_t j = 0, n = rw.block_rows(b); j < n; j++)
        fn(r.side, b, j);
    return;
  }
  // Min-heap of runs by (ts, side).
  auto later = [](const Run& x, const Run& y) {
    return x.ts != y.ts ? x.ts > y.ts : x.side > y.side;
  };
  std::make_heap(runs.begin(), runs.end(), later);
  while (!runs.empty()) {
    std::pop_heap(runs.begin(), runs.end(), later);
    Run& r = runs.back();
    const Blocks& rw = *sides[r.side];
    // Emit rows of this run while it stays ahead of the next best run.
    const Run* next = runs.size() > 1 ? &runs.front() : nullptr;
    for (;;) {
      fn(r.side, r.b, r.j);
      if (++r.j == rw.block_rows(r.b)) {
        r.j = 0;
        if (++r.b == r.end_b)
          break;
      }
      r.ts = rw.col(r.b, kTs)[r.j];
      if (next && later(r, *next))
        break;
    }
    if (r.b == r.end_b)
      runs.pop_back();
    else
      std::push_heap(runs.begin(), runs.end(), later);
  }
}

// ---------------------------------------------------------------------------
// INTERVAL FLATTEN: per lane, the disjoint segments of the live rows with
// COUNT(*) and SUM(v_c) (NULL when no live row has a non-NULL v_c). TP
// semantics for points: a dur = 0 row yields a zero-length segment at its ts
// counting the rows live there plus the points at that ts.
// Output (lane, ts, dur, cnt, s_0 .. s_{NV-1}).
// ---------------------------------------------------------------------------
template <int NV>
struct FlatLane {
  struct Live {
    int64_t end;
    int64_t nulls;
    int64_t v[NV ? NV : 1];
  };
  // Live rows ordered by end, earliest last: for nested slices a new row
  // ends first, so inserts are appends and expiries pop the back.
  Buf<Live> live;
  int64_t lane = 0, cursor = 0, at_ts = 0, inst_cnt = 0;
  bool started = false;
  int64_t live_sum[NV ? NV : 1], live_held[NV ? NV : 1];
  int64_t inst_sum[NV ? NV : 1], inst_held[NV ? NV : 1];

  void Reset(int64_t l) {
    live.Clear();
    lane = l;
    cursor = at_ts = inst_cnt = 0;
    started = false;
    for (int c = 0; c < NV; c++)
      live_sum[c] = live_held[c] = inst_sum[c] = inst_held[c] = 0;
  }
  void Emit(ListOut& out, int64_t ts, int64_t dur, int64_t cnt, bool inst) {
    size_t o = out.Add();
    out.col(0)[o] = lane;
    out.col(1)[o] = ts;
    out.col(2)[o] = dur;
    out.col(3)[o] = cnt;
    for (int c = 0; c < NV; c++) {
      int64_t held = live_held[c] + (inst ? inst_held[c] : 0);
      int64_t sum = live_sum[c] + (inst ? inst_sum[c] : 0);
      out.col(4 + c)[o] = held ? sum : ListOut::kNull;
    }
  }
  static void Apply(const int64_t* v,
                    int64_t nulls,
                    int64_t sign,
                    int64_t* sum,
                    int64_t* held) {
    for (int c = 0; c < NV; c++) {
      int64_t ok = !(nulls >> c & 1);
      sum[c] += sign * ok * v[c];
      held[c] += sign * ok;
    }
  }
  // Emits everything before `time` and moves the cursor there.
  void Advance(ListOut& out, int64_t time) {
    if (inst_cnt) {
      Emit(out, at_ts, 0, static_cast<int64_t>(live.size()) + inst_cnt, true);
      inst_cnt = 0;
      for (int c = 0; c < NV; c++)
        inst_sum[c] = inst_held[c] = 0;
    }
    size_t n = live.size();
    Live* lv = live.data();
    while (n && lv[n - 1].end <= time) {
      int64_t end = lv[n - 1].end;
      if (cursor < end)
        Emit(out, cursor, end - cursor, static_cast<int64_t>(n), false);
      cursor = end;
      for (; n && lv[n - 1].end == end; n--)
        Apply(lv[n - 1].v, lv[n - 1].nulls, -1, live_sum, live_held);
      live.Resize(n);
    }
    if (n && cursor < time)
      Emit(out, cursor, time - cursor, static_cast<int64_t>(n), false);
    cursor = time;
  }
  void Add(ListOut& out,
           int64_t s,
           int64_t e,
           const int64_t* v,
           int64_t nulls) {
    if (!started || s != at_ts) {
      Advance(out, s);
      at_ts = s;
      started = true;
    }
    if (e > s) {
      Apply(v, nulls, 1, live_sum, live_held);
      size_t at = live.size();
      live.Extend(1);
      Live* lv = live.data();
      for (; at > 0 && lv[at - 1].end < e; at--)
        lv[at] = lv[at - 1];
      lv[at].end = e;
      lv[at].nulls = nulls;
      for (int c = 0; c < NV; c++)
        lv[at].v[c] = v[c];
    } else {
      inst_cnt++;
      Apply(v, nulls, 1, inst_sum, inst_held);
    }
  }
  void Finish(ListOut& out) { Advance(out, INT64_MAX); }
};

template <int NV>
void SolveFlatten(State& st, ListOut& out) {
  out.MarkNullable(0);
  for (int c = 0; c < NV; c++)
    out.MarkNullable(4 + c);
  Lanes<FlatLane<NV>>& lanes = TlsLanes<FlatLane<NV>>();
  lanes.Reset();
  const Blocks& rw = st.side[0];
  const Blocks* sides[] = {&rw};
  int64_t v[NV ? NV : 1] = {};
  ForEachInTsOrder(sides, 1, [&](int, size_t b, uint32_t j) {
    for (int c = 0; c < NV; c++)
      v[c] = rw.col(b, kAux + c)[j];
    int64_t nulls = NV ? rw.col(b, kAux + NV)[j] : 0;
    lanes.Get(rw.col(b, kLane)[j])
        .Add(out, rw.col(b, kTs)[j], rw.col(b, kEnd)[j], v, nulls);
  });
  for (size_t l = 0; l < lanes.size(); l++)
    lanes.at(l).Finish(out);
}

// ---------------------------------------------------------------------------
// INTERVAL INTERSECTION of N operands: per lane, one fragment per
// combination of rows (one per operand) that pairwise overlap under TP point
// semantics (a.s < b.ep && b.s < a.ep, ep = max(e, s + 1)). Rows arrive by
// start; a combination is emitted when its last-starting member arrives,
// against the still active rows (ep > start) of the other operands, so each
// combination is found exactly once. Fragment: [start, max(start, min end)).
// Output (lane, ts, dur, r_0 .. r_{N-1}).
// ---------------------------------------------------------------------------
struct IsectLane {
  struct Act {
    int64_t e, ep, id;
  };
  Buf<Act> act[kMaxSides];
  size_t pos[kMaxSides];  // next bucketed row per operand
  int64_t lane = 0;

  void Reset(int64_t l) {
    lane = l;
    for (auto& a : act)
      a.Clear();
  }
  // Adds row (s, e, id) of operand k: emits its combinations with the
  // active rows of the other operands, and keeps it active for later rows
  // unless `keep` is false (no later row can overlap it).
  void Add(ListOut& out,
           int ns,
           int k,
           int64_t s,
           int64_t e,
           int64_t id,
           bool keep) {
    if (keep)
      act[k].Push({e, std::max(e, s + 1), id});
    if (ns == 2) {
      // Retire the other operand's rows that ended before s and pair x with
      // the rest, in one pass.
      Buf<Act>& other = act[1 - k];
      Act* a = other.data();
      size_t n = other.size(), w = 0;
      out.Reserve(n);
      int64_t* c[5] = {out.col(0), out.col(1), out.col(2), out.col(3),
                       out.col(4)};
      size_t r = out.size();
      for (size_t i = 0; i < n; i++) {
        if (a[i].ep <= s)
          continue;
        a[w++] = a[i];
        c[0][r] = lane;
        c[1][r] = s;
        c[2][r] = std::max(s, std::min(e, a[i].e)) - s;
        c[3 + k][r] = id;
        c[4 - k][r] = a[i].id;
        r++;
      }
      other.Resize(w);
      out.Commit(r);
      return;
    }
    // Retire the other operands' rows that ended before s.
    bool none = false;
    for (int o = 0; o < ns; o++) {
      if (o == k)
        continue;
      Act* a = act[o].data();
      size_t w = 0, n = act[o].size();
      for (size_t i = 0; i < n; i++)
        if (a[i].ep > s)
          a[w++] = a[i];
      act[o].Resize(w);
      none |= w == 0;
    }
    if (none)
      return;
    // The new row with every tuple of the others' active rows.
    size_t idx[kMaxSides] = {0, 0, 0, 0};
    for (;;) {
      int64_t me = e;
      for (int o = 0; o < ns; o++)
        if (o != k)
          me = std::min(me, act[o][idx[o]].e);
      size_t r = out.Add();
      out.col(0)[r] = lane;
      out.col(1)[r] = s;
      out.col(2)[r] = std::max(s, me) - s;
      for (int o = 0; o < ns; o++)
        out.col(3 + o)[r] = o == k ? id : act[o][idx[o]].id;
      int o = 0;
      for (; o < ns; o++) {
        if (o == k)
          continue;
        if (++idx[o] < act[o].size())
          break;
        idx[o] = 0;
      }
      if (o == ns)
        return;
    }
  }
};

// The operands of one partition bucketed per lane: rows scattered into
// per-lane slices of {ts, end, id}, stably, so ts-ordered input stays ordered
// (unordered lanes are sorted). Lanes missing from an operand cannot
// intersect and are dropped. Sweeping lane by lane keeps a lane's active rows
// in L1; streaming all lanes at once (as flatten does) loses that locality
// once two operands interleave.
struct Rec {
  int64_t s, e, id;
};
struct Buckets {
  pf::DenseIds lanes;
  Buf<uint32_t> lane_of[kMaxSides];
  Buf<size_t> off[kMaxSides];  // per lane start, nlanes + 1 entries
  Buf<Rec> recs[kMaxSides];
  Buf<uint8_t> live;
  IsectLane sweep;

  void Build(const State& st) {
    const int ns = st.nsides;
    lanes.Reset();
    // Intern lanes and count rows per lane in one pass.
    for (int s = 0; s < ns; s++) {
      const Blocks& rw = st.side[s];
      lane_of[s].Resize(rw.size());
      uint32_t* lo = lane_of[s].data();
      Buf<size_t>& cnt = off[s];
      cnt.Clear();
      for (size_t b = 0; b < rw.num_blocks(); b++) {
        const int64_t* ln = rw.col(b, kLane);
        for (uint32_t i = 0, n = rw.block_rows(b); i < n; i++) {
          uint32_t l = lanes.Intern(ln[i]);
          while (l >= cnt.size())
            cnt.Push(0);
          cnt[l]++;
          *lo++ = l;
        }
      }
    }
    const size_t nl = lanes.size();
    // off[s][l + 1] = rows of lane l (shifted into place below).
    for (int s = 0; s < ns; s++) {
      Buf<size_t>& o = off[s];
      size_t had = o.size();
      o.Resize(nl + 1);
      for (size_t l = had; l < nl; l++)
        o[l] = 0;
      for (size_t l = nl; l > 0; l--)
        o[l] = o[l - 1];
      o[0] = 0;
    }
    live.Resize(nl);
    std::memset(live.data(), 1, nl);
    for (int s = 0; s < ns; s++)
      for (size_t l = 0; l < nl; l++)
        live[l] &= off[s][l + 1] != 0;
    for (int s = 0; s < ns; s++) {
      const Blocks& rw = st.side[s];
      size_t* o = off[s].data();
      for (size_t l = 0; l < nl; l++)
        o[l + 1] = o[l] + (live[l] ? o[l + 1] : 0);
      recs[s].Resize(o[nl]);
      Rec* out = recs[s].data();
      const uint32_t* lo = lane_of[s].data();
      // o[l] is lane l's fill cursor, shifted back afterwards.
      for (size_t b = 0, i = 0; b < rw.num_blocks(); b++) {
        const int64_t* ts = rw.col(b, kTs);
        const int64_t* en = rw.col(b, kEnd);
        const int64_t* id = rw.col(b, kAux);
        for (uint32_t j = 0, n = rw.block_rows(b); j < n; j++, i++)
          if (live[lo[i]])
            out[o[lo[i]]++] = {ts[j], en[j], id[j]};
      }
      for (size_t l = nl; l > 0; l--)
        o[l] = o[l - 1];
      o[0] = 0;
      for (size_t l = 0; l < nl; l++)
        for (size_t i = o[l] + 1; i < o[l + 1]; i++)
          if (out[i].s < out[i - 1].s) {
            std::stable_sort(
                out + o[l], out + o[l + 1],
                [](const Rec& x, const Rec& y) { return x.s < y.s; });
            break;
          }
    }
  }
};

__attribute__((noinline)) Buckets& TlsBuckets() {
  static thread_local Buckets b;
  return b;
}

// Two operands of very different sizes (e.g. depth-0 vs depth-1 slices):
// only the smaller operand is bucketed per lane; the larger one is streamed
// in stored order and each of its rows probes its lane for the rows that
// overlap it. Per lane, a cursor tracks the first row starting at or after
// the probe's ep (moving little between consecutive probes of ts-ordered
// input) and a prefix max of ep bounds the backward scan for overlaps.
struct Probe {
  pf::DenseIds lanes;
  Buf<uint32_t> lane_of;
  Buf<size_t> off;      // per lane start, nlanes + 1 entries
  Buf<Rec> recs;        // by lane, then start
  Buf<int64_t> max_ep;  // per lane prefix max of ep, parallel to recs
  Buf<size_t> cur;      // per lane probe cursor

  void Build(const Blocks& rw) {
    lanes.Reset();
    lane_of.Resize(rw.size());
    uint32_t* lo = lane_of.data();
    off.Clear();
    for (size_t b = 0; b < rw.num_blocks(); b++) {
      const int64_t* ln = rw.col(b, kLane);
      for (uint32_t i = 0, n = rw.block_rows(b); i < n; i++) {
        uint32_t l = lanes.Intern(ln[i]);
        while (l >= off.size())
          off.Push(0);
        off[l]++;
        *lo++ = l;
      }
    }
    const size_t nl = lanes.size();
    off.Push(0);
    for (size_t l = nl; l > 0; l--)
      off[l] = off[l - 1];
    off[0] = 0;
    for (size_t l = 0; l < nl; l++)
      off[l + 1] += off[l];
    recs.Resize(rw.size());
    Rec* out = recs.data();
    lo = lane_of.data();
    for (size_t b = 0, i = 0; b < rw.num_blocks(); b++) {
      const int64_t* ts = rw.col(b, kTs);
      const int64_t* en = rw.col(b, kEnd);
      const int64_t* id = rw.col(b, kAux);
      for (uint32_t j = 0, n = rw.block_rows(b); j < n; j++, i++)
        out[off[lo[i]]++] = {ts[j], en[j], id[j]};
    }
    for (size_t l = nl; l > 0; l--)
      off[l] = off[l - 1];
    off[0] = 0;
    max_ep.Resize(rw.size());
    cur.Resize(nl);
    for (size_t l = 0; l < nl; l++) {
      Rec* r = out + off[l];
      size_t n = off[l + 1] - off[l];
      for (size_t i = 1; i < n; i++)
        if (r[i].s < r[i - 1].s) {
          std::stable_sort(
              r, r + n, [](const Rec& x, const Rec& y) { return x.s < y.s; });
          break;
        }
      int64_t* m = max_ep.data() + off[l];
      int64_t mx = INT64_MIN;
      for (size_t i = 0; i < n; i++)
        m[i] = mx = std::max(mx, std::max(r[i].e, r[i].s + 1));
      cur[l] = 0;
    }
  }
};

__attribute__((noinline)) Probe& TlsProbe() {
  static thread_local Probe p;
  return p;
}

// Use Probe when the larger operand has at least this many times the rows
// of the smaller one.
constexpr size_t kProbeSkew = 4;

void SolveIntersectProbe(State& st, ListOut& out) {
  const int small = st.side[0].size() <= st.side[1].size() ? 0 : 1;
  Probe& pb = TlsProbe();
  pb.Build(st.side[small]);
  const Blocks& big = st.side[1 - small];
  const Rec* recs = pb.recs.data();
  const int64_t* max_ep = pb.max_ep.data();
  const size_t* off = pb.off.data();
  size_t* cur = pb.cur.data();
  // Output columns of the big row's id and the small row's id.
  const int cb = 4 - small, cs = 3 + small;
  for (size_t b = 0; b < big.num_blocks(); b++) {
    const int64_t* ln = big.col(b, kLane);
    const int64_t* ts = big.col(b, kTs);
    const int64_t* en = big.col(b, kEnd);
    const int64_t* id = big.col(b, kAux);
    for (uint32_t j = 0, n = big.block_rows(b); j < n; j++) {
      uint32_t l = pb.lanes.Find(ln[j]);
      if (l == pf::DenseIds::kNone)
        continue;
      const int64_t s = ts[j], e = en[j], ep = std::max(e, s + 1);
      // hi: first row of the lane starting at or after ep.
      const size_t lo = off[l], end = off[l + 1];
      size_t hi = lo + cur[l];
      while (hi < end && recs[hi].s < ep)
        hi++;
      while (hi > lo && recs[hi - 1].s >= ep)
        hi--;
      cur[l] = hi - lo;
      // Rows before hi start before ep; those with ep > s overlap.
      for (size_t i = hi; i > lo && max_ep[i - 1] > s; i--) {
        const Rec& r = recs[i - 1];
        if (std::max(r.e, r.s + 1) <= s)
          continue;
        const int64_t fs = std::max(s, r.s);
        size_t k = out.Add();
        out.col(0)[k] = pb.lanes.Key(l);
        out.col(1)[k] = fs;
        out.col(2)[k] = std::max(fs, std::min(e, r.e)) - fs;
        out.col(cb)[k] = id[j];
        out.col(cs)[k] = r.id;
      }
    }
  }
}

void SolveIntersect(State& st, ListOut& out) {
  const int ns = st.nsides;
  for (int s = 0; s < ns; s++)
    if (!st.side[s].size())
      return;
  out.MarkNullable(0);
  for (int s = 0; s < ns; s++)
    out.MarkNullable(3 + s);
  if (ns == 2) {
    size_t n0 = st.side[0].size(), n1 = st.side[1].size();
    if (std::max(n0, n1) >= kProbeSkew * std::min(n0, n1)) {
      SolveIntersectProbe(st, out);
      return;
    }
  }
  Buckets& bk = TlsBuckets();
  bk.Build(st);
  IsectLane& ln = bk.sweep;
  for (size_t l = 0; l < bk.lanes.size(); l++) {
    if (!bk.live[l])
      continue;
    size_t* pos = ln.pos;
    size_t end[kMaxSides];
    for (int s = 0; s < ns; s++) {
      pos[s] = bk.off[s][l];
      end[s] = bk.off[s][l + 1];
    }
    ln.Reset(bk.lanes.Key(static_cast<uint32_t>(l)));
    for (;;) {
      // Next row by start; the lowest operand wins ties.
      int k = -1;
      int64_t best = INT64_MAX;
      for (int s = 0; s < ns; s++)
        if (pos[s] < end[s] && bk.recs[s][pos[s]].s < best) {
          best = bk.recs[s][pos[s]].s;
          k = s;
        }
      if (k < 0)
        break;
      const Rec& x = bk.recs[k][pos[k]++];
      // Keep x active only if a pending row of another operand starts
      // before x ends.
      int64_t ep = std::max(x.e, x.s + 1);
      bool keep = false;
      for (int o = 0; o < ns; o++)
        if (o != k && pos[o] < end[o])
          keep |= bk.recs[o][pos[o]].s < ep;
      ln.Add(out, ns, k, x.s, x.e, x.id, keep);
    }
  }
}

template <void (*SOLVE)(State&, ListOut&), int WIDTH>
void Finalize(duckdb_function_info info,
              duckdb_aggregate_state* src,
              duckdb_vector result,
              idx_t count,
              idx_t offset) {
  int width = WIDTH;
  for (idx_t i = 0; i < count; i++)
    if (State* st = Ops::Get(src[i]))
      width = std::max(width, WIDTH + st->nv);
  ListOut out(width);
  std::vector<size_t> ends(count);
  for (idx_t i = 0; i < count; i++) {
    if (State* st = Ops::Get(src[i])) {
      st->Gather();
      if (st->err) {
        duckdb_aggregate_function_set_error(info, kNegErr);
        return;
      }
      SOLVE(*st, out);
      for (int s = 0; s < kMaxSides; s++)
        st->side[s].Clear();
      pf::Progress::FinishGroup();
    }
    ends[i] = out.size();
  }
  out.Write(result, offset, ends);
}

// pf_interval_flatten_lane_agg(lane, ts, dur, v_0, .., v_{NV-1})
template <int NV>
void UpdateFlatten(duckdb_function_info info,
                   duckdb_data_chunk input,
                   duckdb_aggregate_state* states) {
  idx_t n = duckdb_data_chunk_get_size(input);
  In<int64_t> lane(input, 0), ts(input, 1), dur(input, 2);
  std::vector<In<int64_t>> vals;
  for (int c = 0; c < NV; c++)
    vals.emplace_back(input, 3 + c);
  bool ok = true;
  Ops::Runs(states, n, [&](idx_t r, idx_t e, State& st) {
    st.nv = NV;
    st.side[0].SetWidth(3 + NV + (NV > 0));
    ok &= AppendRun(st.side[0], lane, ts, dur, vals.data(), NV, NV > 0, r, e);
  });
  if (!ok)
    duckdb_aggregate_function_set_error(info, kNegErr);
}

// pf_interval_intersect<NS>_lane_agg(lane, side, id, ts, dur)
template <int NS>
void UpdateIntersect(duckdb_function_info info,
                     duckdb_data_chunk input,
                     duckdb_aggregate_state* states) {
  idx_t n = duckdb_data_chunk_get_size(input);
  In<int64_t> lane(input, 0), id(input, 2), ts(input, 3), dur(input, 4);
  In<int32_t> side(input, 1);
  bool ok = true;
  Ops::Runs(states, n, [&](idx_t r, idx_t e, State& st) {
    st.nsides = NS;
    // Sub-runs of one operand (the operands are fed one after another).
    for (idx_t a = r; a < e;) {
      int32_t sd = side.data[a];
      idx_t b = a + 1;
      while (b < e && side.data[b] == sd)
        b++;
      if (side.Valid(a) && sd >= 0 && sd < NS) {
        st.side[sd].SetWidth(4);
        ok &= AppendRun(st.side[sd], lane, ts, dur, &id, 1, false, a, b);
      }
      a = b;
    }
  });
  if (!ok)
    duckdb_aggregate_function_set_error(info, kNegErr);
}

duckdb_logical_type ListOfBigintStruct(const std::vector<std::string>& names) {
  std::vector<duckdb_logical_type> ts(names.size());
  std::vector<const char*> nm(names.size());
  for (size_t i = 0; i < names.size(); i++) {
    ts[i] = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
    nm[i] = names[i].c_str();
  }
  duckdb_logical_type st =
      duckdb_create_struct_type(ts.data(), nm.data(), ts.size());
  for (auto& t : ts)
    duckdb_destroy_logical_type(&t);
  duckdb_logical_type l = duckdb_create_list_type(st);
  duckdb_destroy_logical_type(&st);
  return l;
}

duckdb_aggregate_function MakeAgg(const char* name,
                                  const std::vector<duckdb_type>& params,
                                  const std::vector<std::string>& out,
                                  duckdb_aggregate_update_t update,
                                  duckdb_aggregate_finalize_t finalize) {
  duckdb_aggregate_function f = duckdb_create_aggregate_function();
  duckdb_aggregate_function_set_name(f, name);
  for (duckdb_type p : params) {
    duckdb_logical_type t = duckdb_create_logical_type(p);
    duckdb_aggregate_function_add_parameter(f, t);
    duckdb_destroy_logical_type(&t);
  }
  duckdb_logical_type ret = ListOfBigintStruct(out);
  duckdb_aggregate_function_set_return_type(f, ret);
  duckdb_destroy_logical_type(&ret);
  duckdb_aggregate_function_set_functions(f, Ops::Size, Ops::Init, update,
                                          Ops::Combine, finalize);
  duckdb_aggregate_function_set_destructor(f, Ops::Destroy);
  duckdb_aggregate_function_set_special_handling(f);
  return f;
}

bool RegisterSet(duckdb_connection conn,
                 const char* name,
                 std::vector<duckdb_aggregate_function> fs) {
  duckdb_aggregate_function_set set =
      duckdb_create_aggregate_function_set(name);
  bool ok = true;
  for (auto& f : fs) {
    ok &= duckdb_add_aggregate_function_to_set(set, f) == DuckDBSuccess;
    duckdb_destroy_aggregate_function(&f);
  }
  ok &= duckdb_register_aggregate_function_set(conn, set) == DuckDBSuccess;
  duckdb_destroy_aggregate_function_set(&set);
  return ok;
}

// pf_interval_flatten_lane_agg(lane, ts, dur [, v_0 .. v_7]):
// LIST<STRUCT(lane, ts, dur, cnt, s_0 ..)>.
template <int NV>
void AddFlatten(std::vector<duckdb_aggregate_function>* fs) {
  std::vector<duckdb_type> p(3 + NV, DUCKDB_TYPE_BIGINT);
  std::vector<std::string> out = {"lane", "ts", "dur", "cnt"};
  for (int c = 0; c < NV; c++)
    out.push_back("s" + std::to_string(c));
  fs->push_back(MakeAgg("pf_interval_flatten_lane_agg", p, out,
                        UpdateFlatten<NV>, Finalize<SolveFlatten<NV>, 4>));
  if constexpr (NV < kMaxSums)
    AddFlatten<NV + 1>(fs);
}

// pf_interval_intersect<N>_lane_agg(lane, side, id, ts, dur):
// LIST<STRUCT(lane, ts, dur, r0 .. r<N-1>)>.
template <int NS>
bool RegisterIntersect(duckdb_connection conn) {
  std::vector<duckdb_type> p = {DUCKDB_TYPE_BIGINT, DUCKDB_TYPE_INTEGER,
                                DUCKDB_TYPE_BIGINT, DUCKDB_TYPE_BIGINT,
                                DUCKDB_TYPE_BIGINT};
  std::vector<std::string> out = {"lane", "ts", "dur"};
  for (int s = 0; s < NS; s++)
    out.push_back("r" + std::to_string(s));
  std::string name = "pf_interval_intersect" + std::to_string(NS) + "_lane_agg";
  bool ok = RegisterSet(conn, name.c_str(),
                        {MakeAgg(name.c_str(), p, out, UpdateIntersect<NS>,
                                 Finalize<SolveIntersect, 3 + NS>)});
  if constexpr (NS < kMaxSides)
    ok &= RegisterIntersect<NS + 1>(conn);
  return ok;
}

bool Register(duckdb_connection conn) {
  std::vector<duckdb_aggregate_function> fs;
  AddFlatten<0>(&fs);
  return RegisterSet(conn, "pf_interval_flatten_lane_agg", fs) &&
         RegisterIntersect<2>(conn);
}

}  // namespace ivl

}  // namespace

bool RegisterIntervalFunctions(duckdb_connection conn) {
  bool ok = true;
  ok &= ivl::Register(conn);
  // (side, row, ts, dur, n_operands) -> [(ts, dur, rows[n])]
  ok &= Register(conn, "pf_interval_intersect_agg", Generic(true),
                 OutType({"ts", "dur"}, "rows"), UpdateGeneric,
                 FinalizeWith<2, true, SolveIntersect>);
  ok &= Register(conn, "pf_interval_intersect2_agg", Generic(false),
                 OutType({"ts", "dur", "r0", "r1"}, nullptr), UpdateGeneric,
                 FinalizeWith<4, false, SolveIntersect2>);
  ok &= Register(conn, "pf_interval_intersect3_agg", Generic(false),
                 OutType({"ts", "dur", "r0", "r1", "r2"}, nullptr),
                 UpdateGeneric, FinalizeWith<5, false, SolveIntersect3>);
  ok &= Register(conn, "pf_intervals_from_events_agg", Generic(true),
                 OutType({"id", "ts", "dur"}, nullptr), UpdateFromEvents,
                 FinalizeWith<3, false, SolveFromEvents>);
  ok &= Register(conn, "pf_interval_union_agg", Generic(true),
                 OutType({"ts", "dur"}, "rows"), UpdateGeneric,
                 FinalizeWith<2, true, SolveUnion>);
  // (side, row, ts, dur) -> [(ts, dur, rows[])]
  ok &= Register(conn, "pf_interval_flatten_agg", Generic(false),
                 OutType({"ts", "dur"}, "rows"), UpdateGeneric,
                 FinalizeWith<2, true, SolveFlatten>);
  ok &= Register(conn, "pf_interval_merge_agg", Generic(false),
                 OutType({"ts", "dur"}, "rows"), UpdateGeneric,
                 FinalizeWith<2, true, SolveMerge>);
  ok &= Register(conn, "pf_interval_merge_count_agg",
                 {T(DUCKDB_TYPE_BIGINT), T(DUCKDB_TYPE_BIGINT)},
                 OutType({"ts", "dur", "cnt"}, nullptr), UpdateFlattenSum0,
                 FinalizeWith<3, false, SolveMergeCount>);
  ok &= Register(conn, "pf_interval_split_agg", Generic(false),
                 OutType({"ts", "dur", "r0", "r1"}, nullptr), UpdateGeneric,
                 FinalizeWith<4, false, SolveSplit>);
  ok &= Register(conn, "pf_interval_subtract_agg", Generic(false),
                 OutType({"ts", "dur", "r0"}, nullptr), UpdateGeneric,
                 FinalizeWith<3, false, SolveSubtract>);
  // (side, row, ts, dur, rel | LEFT<<8) -> [(r0, r1)]
  ok &= Register(conn, "pf_interval_join_agg", Generic(true),
                 OutType({"r0", "r1"}, nullptr), UpdateGeneric,
                 FinalizeWith<2, false, SolveJoin>);
  ok &= Register(conn, "pf_interval_find_agg", Generic(true),
                 OutType({"r0", "r1"}, nullptr), UpdateGeneric,
                 FinalizeWith<2, false, SolveFind>);
  ok &= Register(conn, "pf_interval_keep_agg", Generic(true),
                 OutType({"r0"}, nullptr), UpdateGeneric,
                 FinalizeWith<1, false, SolveKeep>);
  {
    duckdb_scalar_function sf = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(sf, "pf_interval_quantize_segs");
    for (int i = 0; i < 4; i++) {
      duckdb_logical_type b = BigInt();
      duckdb_scalar_function_add_parameter(sf, b);
      duckdb_destroy_logical_type(&b);
    }
    duckdb_logical_type ret = OutType({"ts", "dur", "bucket"}, nullptr);
    duckdb_scalar_function_set_return_type(sf, ret);
    duckdb_scalar_function_set_function(sf, QuantizeSegsScalar);
    duckdb_scalar_function_set_special_handling(sf);
    ok &= duckdb_register_scalar_function(conn, sf) == DuckDBSuccess;
    duckdb_destroy_scalar_function(&sf);
    duckdb_destroy_logical_type(&ret);
  }
  ok &= Register(
      conn, "pf_interval_merge_consecutive_count_agg",
      {T(DUCKDB_TYPE_BIGINT), T(DUCKDB_TYPE_BIGINT), T(DUCKDB_TYPE_BIGINT),
       T(DUCKDB_TYPE_BIGINT), T(DUCKDB_TYPE_BIGINT)},
      OutType({"ts", "dur", "r0", "cnt"}, nullptr), UpdateQuantize,
      FinalizeWith<4, false, SolveMergeConsecutiveCount>);
  return ok;
}
