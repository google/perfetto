// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Shared declarations of the interval, tree and graph operators.
#ifndef PERFETTO_BIGTRACE_DUCKDB_EXT_COMMON_H_
#define PERFETTO_BIGTRACE_DUCKDB_EXT_COMMON_H_

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "extension.h"  // no-include-violation-check

// Compiles `|>` chain syntax (pipe.cc). Returns false, setting `error`, if
// `sql` is not valid.
bool CompilePipe(const std::string& sql, std::string* out, std::string* error);

// SQL generators shared between sql_gen.cc and pipe.cc.
std::string GenIntervalFlattenSql(const std::string& rel,
                                  const std::vector<std::string>& aggs,
                                  const std::vector<std::string>& per,
                                  int64_t threads);
std::string GenIntervalIntersectionSql(
    const std::vector<std::string>& rels,
    const std::vector<std::string>& aliases,
    const std::vector<std::string>& per,
    bool full_row,
    int64_t threads,
    const std::vector<std::vector<std::string>>& needed_cols = {});
std::string GenTreeAccumulateSql(const std::string& tbl,
                                 const std::string& direction,
                                 const std::vector<std::string>& aggs,
                                 const std::vector<std::string>& per,
                                 bool full_row,
                                 int64_t threads);
std::string GenTreeFilterSql(const std::string& op,
                             const std::string& tbl,
                             const std::string& marker,
                             const std::vector<std::string>& per,
                             const std::string& over,
                             bool full_row,
                             int64_t threads);
std::string GenGraphTreeSql(const std::string& kind,
                            const std::string& nodes,
                            const std::string& edges,
                            const std::string& seeds,
                            const std::vector<std::string>& per,
                            bool full_row,
                            int64_t threads);

// Building blocks of the partition kernels.
//
// Every operator is an aggregate grouped by the trace partition (GROUP BY
// _trace_uuid): DuckDB's hash aggregate routes each input row to its
// partition's state, Update appends the rows column-wise, Combine splices the
// per-thread parts, and Finalize solves the whole partition in one pass and
// returns LIST<STRUCT(...)>, which the SQL layer unnests. DuckDB finalizes
// radix partitions in parallel, so partitions (traces) are solved
// concurrently without any cross-thread merge.
namespace pf {

// A flat input column (CAPIAggregateUpdate flattens every input vector).
template <typename T>
struct In {
  const T* data;
  const uint64_t* valid;  // nullptr when the chunk has no NULLs
  In(duckdb_data_chunk c, idx_t i) {
    duckdb_vector v = duckdb_data_chunk_get_vector(c, i);
    data = static_cast<const T*>(duckdb_vector_get_data(v));
    valid = duckdb_vector_get_validity(v);
  }
  bool Valid(idx_t r) const {
    return !valid || ((valid[r >> 6] >> (r & 63)) & 1);
  }
  // True when rows [r, e) have no NULL.
  bool AllValid(idx_t r, idx_t e) const {
    if (!valid || r >= e)
      return true;
    for (idx_t w = r >> 6; w <= (e - 1) >> 6; w++) {
      uint64_t inv = ~valid[w];
      if (w == r >> 6)
        inv &= ~0ull << (r & 63);
      if (w == (e - 1) >> 6 && (e & 63))
        inv &= (1ull << (e & 63)) - 1;
      if (inv)
        return false;
    }
    return true;
  }
};

// Growable POD array. realloc of large blocks is an mremap in glibc, so
// growth does not copy; elements are never value-initialized.
template <typename T>
class Buf {
 public:
  Buf() = default;
  ~Buf() { std::free(p_); }
  Buf(const Buf&) = delete;
  Buf& operator=(const Buf&) = delete;
  Buf(Buf&& o) noexcept : p_(o.p_), n_(o.n_), cap_(o.cap_) {
    o.p_ = nullptr;
    o.n_ = o.cap_ = 0;
  }
  Buf& operator=(Buf&& o) noexcept {
    std::swap(p_, o.p_);
    std::swap(n_, o.n_);
    std::swap(cap_, o.cap_);
    return *this;
  }

  size_t size() const { return n_; }
  bool empty() const { return n_ == 0; }
  T* data() { return p_; }
  const T* data() const { return p_; }
  T& operator[](size_t i) { return p_[i]; }
  const T& operator[](size_t i) const { return p_[i]; }
  T& back() { return p_[n_ - 1]; }

  void Reserve(size_t c) {
    if (c <= cap_)
      return;
    size_t nc = cap_ ? cap_ : 1024;
    while (nc < c)
      nc *= 2;
    void* np = std::realloc(p_, nc * sizeof(T));
    if (!np)
      std::abort();
    p_ = static_cast<T*>(np);
    cap_ = nc;
  }
  // Returns room for k more elements at the end and counts them as used.
  T* Extend(size_t k) {
    Reserve(n_ + k);
    T* r = p_ + n_;
    n_ += k;
    return r;
  }
  void Resize(size_t n) {
    Reserve(n);
    n_ = n;
  }
  void Append(const T* src, size_t k) {
    if (k)
      std::memcpy(Extend(k), src, k * sizeof(T));
  }
  void Push(const T& v) {
    if (n_ == cap_)
      Reserve(n_ + 1);
    p_[n_++] = v;
  }
  void Clear() { n_ = 0; }
  void Free() {
    std::free(p_);
    p_ = nullptr;
    n_ = cap_ = 0;
  }

 private:
  T* p_ = nullptr;
  size_t n_ = 0;
  size_t cap_ = 0;
};

// Aggregate state = one heap object per group, created on first row. The
// state type S provides Absorb(S&&), called by Combine to splice a part.
template <typename S>
struct StateOps {
  static S*& Get(duckdb_aggregate_state s) { return *reinterpret_cast<S**>(s); }
  static idx_t Size(duckdb_function_info) { return sizeof(S*); }
  static void Init(duckdb_function_info, duckdb_aggregate_state s) {
    Get(s) = nullptr;
  }
  static void Destroy(duckdb_aggregate_state* s, idx_t n) {
    for (idx_t i = 0; i < n; i++) {
      delete Get(s[i]);
      Get(s[i]) = nullptr;
    }
  }
  static void Combine(duckdb_function_info,
                      duckdb_aggregate_state* src,
                      duckdb_aggregate_state* dst,
                      idx_t n) {
    for (idx_t i = 0; i < n; i++) {
      S*& s = Get(src[i]);
      if (!s)
        continue;
      S*& d = Get(dst[i]);
      if (!d) {
        d = s;
      } else {
        d->Absorb(std::move(*s));
        delete s;
      }
      s = nullptr;
    }
  }
  // Calls fn(first, end, state) for each run of rows sharing one state.
  template <typename F>
  static void Runs(duckdb_aggregate_state* states, idx_t n, F&& fn) {
    for (idx_t r = 0; r < n;) {
      duckdb_aggregate_state sp = states[r];
      idx_t e = r + 1;
      while (e < n && states[e] == sp)
        e++;
      S*& st = Get(sp);
      if (!st)
        st = new S();
      fn(r, e, *st);
      r = e;
    }
  }
};

// Append-only row store of one partition: blocks of kRows rows x `width`
// int64 columns (column-major inside a block). Appends never move data, and
// splicing another store (Combine) only moves block pointers. A row is
// addressed by its handle (block << kShift | row in block).
// Recycles row-store blocks across queries: freed blocks go to a per-width
// free list (up to kCapBytes in total) instead of back to malloc, so repeated
// queries reuse warm pages instead of page-faulting fresh ones (a fault per
// 4 KB page was ~25% of a tree kernel's time).
class BlockPool {
 public:
  static constexpr size_t kCapBytes = size_t{1} << 30;
  static constexpr int kMaxWidth = 16;

  static void* Get(int width, size_t bytes) {
    BlockPool& p = Instance();
    {
      std::lock_guard<std::mutex> l(p.mu_);
      auto& fl = p.free_[width];
      if (!fl.empty()) {
        void* b = fl.back();
        fl.pop_back();
        p.cached_ -= bytes;
        return b;
      }
    }
    void* b = std::malloc(bytes);
    if (!b)
      std::abort();
    return b;
  }
  static void Put(void* b, int width, size_t bytes) {
    BlockPool& p = Instance();
    {
      std::lock_guard<std::mutex> l(p.mu_);
      if (p.cached_ + bytes <= kCapBytes) {
        p.free_[width].push_back(b);
        p.cached_ += bytes;
        return;
      }
    }
    std::free(b);
  }

 private:
  static BlockPool& Instance() {
    static BlockPool* p = new BlockPool();  // never destroyed
    return *p;
  }
  std::mutex mu_;
  std::vector<void*> free_[kMaxWidth + 1];
  size_t cached_ = 0;
};

class Blocks {
 public:
  static constexpr int kShift = 11;
  static constexpr size_t kRows = size_t{1} << kShift;  // = DuckDB's vector

  Blocks() = default;
  ~Blocks() { Clear(); }
  Blocks(const Blocks&) = delete;
  Blocks& operator=(const Blocks&) = delete;
  Blocks(Blocks&& o) noexcept
      : blocks_(std::move(o.blocks_)), width_(o.width_), rows_(o.rows_) {
    o.rows_ = 0;
  }

  void SetWidth(int w) { width_ = w; }
  int width() const { return width_; }
  size_t size() const { return rows_; }
  size_t num_blocks() const { return blocks_.size(); }
  uint32_t block_rows(size_t b) const { return blocks_[b]->n; }
  const int64_t* col(size_t b, int c) const {
    return blocks_[b]->data + c * kRows;
  }
  int64_t At(uint64_t handle, int c) const {
    return blocks_[handle >> kShift]->data[c * kRows + (handle & (kRows - 1))];
  }

  // Room for up to `want` rows in the last block: returns the column base
  // pointers' block and the first free row; Commit(k) counts k rows.
  int64_t* Room(size_t want, size_t* first, size_t* fit) {
    if (blocks_.empty() || blocks_.back()->n == kRows) {
      blocks_.push_back(static_cast<Block*>(BlockPool::Get(width_, Bytes())));
      blocks_.back()->n = 0;
    }
    Block* b = blocks_.back();
    *first = b->n;
    *fit = std::min(want, kRows - b->n);
    return b->data;
  }
  void Commit(size_t k) {
    blocks_.back()->n += static_cast<uint32_t>(k);
    rows_ += k;
  }
  void Splice(Blocks&& o) {
    blocks_.insert(blocks_.end(), o.blocks_.begin(), o.blocks_.end());
    rows_ += o.rows_;
    o.blocks_.clear();
    o.rows_ = 0;
  }
  void Clear() {
    for (Block* b : blocks_)
      BlockPool::Put(b, width_, Bytes());
    blocks_.clear();
    rows_ = 0;
  }

 private:
  struct Block {
    uint32_t n;
    uint32_t pad;
    int64_t data[1];
  };
  size_t Bytes() const {
    return sizeof(Block) + kRows * width_ * sizeof(int64_t);
  }
  std::vector<Block*> blocks_;
  int width_ = 0;
  size_t rows_ = 0;
};

// Output of one Finalize call: LIST<STRUCT(c0 BIGINT, ..)> rows of every
// group, buffered column-wise in thread-local arrays reused across calls and
// copied into the result vector once. Nullable columns mark NULL as kNull.
class ListOut {
 public:
  static constexpr int64_t kNull = INT64_MIN;
  static constexpr int kMaxWidth = 16;

  explicit ListOut(int width) : width_(width), s_(Tls()) {
    // Columns first used by a wider output than before get the shared
    // capacity.
    for (int c = 0; c < width; c++)
      if (!s_.cols[c] && s_.cap)
        s_.cols[c] = Alloc(nullptr, s_.cap);
  }
  ListOut(const ListOut&) = delete;
  ListOut& operator=(const ListOut&) = delete;

  size_t size() const { return n_; }
  // Appends one row; returns its index.
  size_t Add() {
    if (n_ == s_.cap)
      Grow(n_ + 1);
    return n_++;
  }
  // Appends k rows; returns the first index.
  size_t Extend(size_t k) {
    if (n_ + k > s_.cap)
      Grow(n_ + k);
    size_t at = n_;
    n_ += k;
    return at;
  }
  int64_t* col(int c) { return s_.cols[c]; }
  // Guarantees room for k more rows: col() pointers stay valid for rows
  // [size(), size() + k) until the next Add / Extend / Reserve; Commit(n)
  // then sets the size.
  void Reserve(size_t k) {
    if (n_ + k > s_.cap)
      Grow(n_ + k);
  }
  void Commit(size_t n) { n_ = n; }
  void MarkNullable(int c) { nullable_ |= 1u << c; }

  // Writes the rows; group g owns rows [ends[g-1], ends[g]).
  void Write(duckdb_vector result,
             idx_t offset,
             const std::vector<size_t>& ends) const {
    idx_t base = duckdb_list_vector_get_size(result);
    duckdb_list_vector_reserve(result, base + n_);
    duckdb_vector child = duckdb_list_vector_get_child(result);
    for (int c = 0; c < width_; c++) {
      duckdb_vector v = duckdb_struct_vector_get_child(child, c);
      int64_t* dst = static_cast<int64_t*>(duckdb_vector_get_data(v));
      const int64_t* src = s_.cols[c];
      if (n_)
        std::memcpy(dst + base, src, n_ * sizeof(int64_t));
      if (!(nullable_ >> c & 1))
        continue;
      uint64_t* val = nullptr;
      for (size_t i = 0; i < n_; i++) {
        if (src[i] != kNull)
          continue;
        if (!val) {
          duckdb_vector_ensure_validity_writable(v);
          val = duckdb_vector_get_validity(v);
        }
        duckdb_validity_set_row_invalid(val, base + i);
      }
    }
    auto* entries =
        static_cast<duckdb_list_entry*>(duckdb_vector_get_data(result));
    size_t prev = 0;
    for (size_t g = 0; g < ends.size(); g++) {
      entries[offset + g].offset = base + prev;
      entries[offset + g].length = ends[g] - prev;
      prev = ends[g];
    }
    duckdb_list_vector_set_size(result, base + n_);
  }

 private:
  struct Storage {
    int64_t* cols[kMaxWidth] = {};
    size_t cap = 0;
  };
  static Storage& Tls() {
    static thread_local Storage s;
    return s;
  }
  static int64_t* Alloc(int64_t* p, size_t n) {
    void* r = std::realloc(p, n * sizeof(int64_t));
    if (!r)
      std::abort();
    return static_cast<int64_t*>(r);
  }
  void Grow(size_t need) {
    size_t nc = s_.cap ? s_.cap : 4096;
    while (nc < need)
      nc *= 2;
    for (int c = 0; c < kMaxWidth; c++)
      if (c < width_ || s_.cols[c])
        s_.cols[c] = Alloc(s_.cols[c], nc);
    s_.cap = nc;
  }

  int width_;
  uint32_t nullable_ = 0;
  size_t n_ = 0;
  Storage& s_;
};

// Dense renumbering of the int64 keys of one partition (lanes, node ids):
// small non-negative keys index a reusable direct table, others go through
// an open-addressing hash table. Reset() is O(keys seen).
class DenseIds {
 public:
  static constexpr uint32_t kNone = UINT32_MAX;
  static constexpr int64_t kDirect = 1 << 20;

  DenseIds() : direct_(kDirect, kNone) {}

  // Returns the dense id of k, assigning the next one if new.
  inline uint32_t Intern(int64_t k) {
    if (static_cast<uint64_t>(k) < static_cast<uint64_t>(kDirect)) {
      uint32_t& slot = direct_[k];
      if (slot == kNone) {
        slot = static_cast<uint32_t>(keys_.size());
        keys_.push_back(k);
      }
      return slot;
    }
    return InternHashed(k);
  }
  // Dense id of k or kNone.
  uint32_t Find(int64_t k) const {
    if (static_cast<uint64_t>(k) < static_cast<uint64_t>(kDirect))
      return direct_[k];
    if (hkeys_.empty())
      return kNone;
    size_t mask = hkeys_.size() - 1;
    for (size_t h = Hash(k) & mask;; h = (h + 1) & mask) {
      if (hvals_[h] == kNone)
        return kNone;
      if (hkeys_[h] == k)
        return hvals_[h];
    }
  }
  size_t size() const { return keys_.size(); }
  int64_t Key(uint32_t id) const { return keys_[id]; }
  void Reset() {
    for (int64_t k : keys_)
      if (static_cast<uint64_t>(k) < static_cast<uint64_t>(kDirect))
        direct_[k] = kNone;
    keys_.clear();
    if (hused_) {
      std::fill(hvals_.begin(), hvals_.end(), kNone);
      hused_ = 0;
    }
  }

 private:
  static size_t Hash(int64_t k) {
    uint64_t x = static_cast<uint64_t>(k) * 0x9E3779B97F4A7C15ull;
    return static_cast<size_t>(x ^ (x >> 32));
  }
  __attribute__((noinline)) uint32_t InternHashed(int64_t k) {
    if (hkeys_.empty() || (hused_ + 1) * 2 > hkeys_.size())
      Rehash();
    size_t mask = hkeys_.size() - 1;
    for (size_t h = Hash(k) & mask;; h = (h + 1) & mask) {
      if (hvals_[h] == kNone) {
        hkeys_[h] = k;
        hvals_[h] = static_cast<uint32_t>(keys_.size());
        hused_++;
        keys_.push_back(k);
        return hvals_[h];
      }
      if (hkeys_[h] == k)
        return hvals_[h];
    }
  }
  void Rehash() {
    size_t n = hkeys_.empty() ? 1024 : hkeys_.size() * 2;
    std::vector<int64_t> ok;
    std::vector<uint32_t> ov;
    ok.swap(hkeys_);
    ov.swap(hvals_);
    hkeys_.assign(n, 0);
    hvals_.assign(n, kNone);
    for (size_t i = 0; i < ok.size(); i++) {
      if (ov[i] == kNone)
        continue;
      for (size_t h = Hash(ok[i]) & (n - 1);; h = (h + 1) & (n - 1)) {
        if (hvals_[h] == kNone) {
          hkeys_[h] = ok[i];
          hvals_[h] = ov[i];
          break;
        }
      }
    }
  }

  std::vector<uint32_t> direct_;
  std::vector<int64_t> keys_;
  std::vector<int64_t> hkeys_;
  std::vector<uint32_t> hvals_;
  size_t hused_ = 0;
};

}  // namespace pf

#endif  // PERFETTO_BIGTRACE_DUCKDB_EXT_COMMON_H_
