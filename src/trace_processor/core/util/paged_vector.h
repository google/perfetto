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

#ifndef SRC_TRACE_PROCESSOR_CORE_UTIL_PAGED_VECTOR_H_
#define SRC_TRACE_PROCESSOR_CORE_UTIL_PAGED_VECTOR_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/core/util/page_store.h"
#include "src/trace_processor/core/util/slab.h"

namespace perfetto::trace_processor::core {

// A growable, page-backed vector of trivially-copyable elements.
//
// PagedVector is the storage primitive used by dataframe columns and other
// row-shaped data structures. It is structured as a sequence of frozen byte
// slab pages plus a single growable tail slab where appends land. While the
// tail is the only page, the layout is contiguous and `data()` / `begin()` /
// `end()` behave like a plain vector — there is no page-walk overhead in the
// common single-page case.
//
// ## Type erasure
//
// All `PagedVector<T>` instantiations have **identical memory layout**. The
// internal storage is byte-typed (`Slab<uint8_t>`) and all internal sizes and
// offsets are tracked in bytes. The public API (`size()`, `operator[]`, etc.)
// translates between bytes and T-units via `sizeof(T)`.
//
// This allows a `PagedVector<uint8_t>` (used as raw byte storage by callers
// like the tree engine) to be `reinterpret_cast` to `PagedVector<X>*` for any
// trivially-copyable X, and accessed as a typed view of the same bytes. The
// underlying storage is allocated with `kStorageAlignment` (8) so any T up to
// 8 bytes wide is aligned correctly.
//
// ## Multi-page extension
//
// The multi-page extension exists to support spilling pages out of RAM (e.g.
// to disk) without requiring changes to consumers that operate on a single
// contiguous range. Callers that need to handle the multi-page shape use
// `num_pages()` and `page(i)` to walk pages explicitly.
template <typename T>
class PagedVector {
 public:
  // Capacity granularity, in bytes. Slabs are sized as a multiple of this.
  // Defined as 64 * sizeof(T) so the granularity is "64 elements" in T-units
  // — matching FlexVector's behaviour and giving SIMD-friendly alignment.
  // For T = uint8_t (the byte-typed view used by tree storage) this is 64
  // bytes; for T = int64_t / double it's 512 bytes.
  static constexpr size_t kCapacityMultipleBytes = 64 * sizeof(T);

  // Alignment for the underlying byte storage. 8 bytes covers all of
  // uint32_t, int32_t, int64_t, double, StringPool::Id (4 bytes) and any
  // future trivially-copyable T up to 8 bytes wide.
  static constexpr size_t kStorageAlignment = 8;

  // Growth factor for the tail when it runs out of capacity.
  static constexpr double kGrowthFactor = 1.5;

  // Fixed page size (in bytes). The tail auto-seals into a frozen page when
  // it crosses this threshold, then a fresh tail starts. Picked at 4 MiB so:
  //   - Small columns (< 4 MiB) live in a single page — zero paging overhead.
  //   - Large columns split into many pages that can be evicted to disk.
  //   - Per-page metadata (handle, endpoints, slab pointer) is ~64 bytes,
  //     so even a 100 GiB column has < 2 MiB of page-side metadata.
  // No per-instance override in production; tests can call
  // `SetPageBytesForTesting` to verify multi-page behaviour without
  // having to push 4 MiB of data.
  static constexpr uint64_t kPageBytes = 4ull * 1024 * 1024;

  static_assert(std::is_trivially_destructible_v<T>,
                "PagedVector elements must be trivially destructible");
  static_assert(std::is_trivially_copyable_v<T>,
                "PagedVector elements must be trivially copyable");
  static_assert(alignof(T) <= kStorageAlignment,
                "PagedVector storage alignment is too small for T");

  using value_type = T;
  using size_type = uint64_t;
  using difference_type = std::ptrdiff_t;
  using reference = T&;
  using const_reference = const T&;
  using pointer = T*;
  using const_pointer = const T*;
  using iterator = T*;
  using const_iterator = const T*;

  // Constructs an empty vector. The given `store` is the column's backing
  // tier for evicted pages AND its budget participant — registration
  // happens immediately. Production code MUST pass a real session-level
  // store; tests can rely on the default `PageStore::TestingSingleton()`
  // (in-memory, no real eviction, but correct accounting). The store MUST
  // outlive this PagedVector.
  explicit PagedVector(PageStore* store = PageStore::TestingSingleton()) {
    PERFETTO_CHECK(store != nullptr);
    AttachPageStoreInternal(store);
  }

  // Destructor: unregisters from the PageStore (if attached) and updates
  // resident-byte accounting so the store doesn't double-count bytes about
  // to be freed. Reaching this with a dead `page_store_` is a lifetime
  // bug — PageStore must outlive every PagedVector attached to it.
  ~PagedVector() {
    if (page_store_ != nullptr) {
      uint64_t resident = ComputeResidentBytes();
      if (resident > 0) {
        page_store_->OnResidentDelta(-static_cast<int64_t>(resident));
      }
      page_store_->Unregister(evictee_handle_);
    }
  }

  // Move ctor / move-assign: transfer all state, including the page store
  // registration; the store's entry has its `ctx` rebound to the new
  // address so further evictions hit the right object.
  PagedVector(PagedVector&& other) noexcept
      : frozen_pages_(std::move(other.frozen_pages_)),
        frozen_handles_(std::move(other.frozen_handles_)),
        frozen_page_endpoints_(std::move(other.frozen_page_endpoints_)),
        frozen_byte_total_(other.frozen_byte_total_),
        page_store_(other.page_store_),
        tail_(std::move(other.tail_)),
        tail_byte_size_(other.tail_byte_size_),
        page_bytes_(other.page_bytes_),
        evictee_handle_(other.evictee_handle_) {
    if (page_store_ != nullptr) {
      page_store_->RebindCtx(evictee_handle_, this);
    }
    other.page_store_ = nullptr;
    other.frozen_byte_total_ = 0;
    other.tail_byte_size_ = 0;
  }
  PagedVector& operator=(PagedVector&& other) noexcept {
    if (this != &other) {
      this->~PagedVector();
      new (this) PagedVector(std::move(other));
    }
    return *this;
  }
  PagedVector(const PagedVector&) = delete;
  PagedVector& operator=(const PagedVector&) = delete;

  // Allocates a new PagedVector with the specified initial tail capacity (in
  // T-units), attached to `store` (must be non-null; defaults to the test
  // singleton — production code passes a real store).
  static PagedVector<T> CreateWithCapacity(
      uint64_t capacity,
      PageStore* store = PageStore::TestingSingleton()) {
    PERFETTO_CHECK(store != nullptr);
    return PagedVector(RoundUpBytes(capacity * sizeof(T)), 0, store);
  }

  // Allocates a new PagedVector with the specified initial size, attached
  // to `store` (must be non-null; defaults to the test singleton).
  // Values are not initialized.
  static PagedVector<T> CreateWithSize(
      uint64_t size,
      PageStore* store = PageStore::TestingSingleton()) {
    PERFETTO_CHECK(store != nullptr);
    return PagedVector(RoundUpBytes(size * sizeof(T)), size * sizeof(T),
                       store);
  }

  // Adds `value` to the end of the vector.
  PERFETTO_ALWAYS_INLINE void push_back(T value) {
    if (PERFETTO_UNLIKELY(tail_byte_size_ + sizeof(T) > tail_.size())) {
      IncreaseCapacity();
    }
    std::memcpy(tail_.data() + tail_byte_size_, &value, sizeof(T));
    tail_byte_size_ += sizeof(T);
    // Auto-seal: once the tail reaches the page-byte threshold, freeze it
    // into a frozen page and start a fresh tail.
    if (PERFETTO_UNLIKELY(tail_byte_size_ >= page_bytes_)) {
      Seal();
    }
  }

  // Adds `count` copies of `value` to the end of the vector.
  PERFETTO_ALWAYS_INLINE void push_back_multiple(T value, uint64_t count) {
    while (PERFETTO_UNLIKELY(tail_byte_size_ + count * sizeof(T) >
                             tail_.size())) {
      IncreaseCapacity();
    }
    for (uint64_t i = 0; i < count; ++i) {
      std::memcpy(tail_.data() + tail_byte_size_, &value, sizeof(T));
      tail_byte_size_ += sizeof(T);
    }
  }

  // Removes the last element from the vector. Should not be called on an
  // empty vector. Only supported in single-page mode.
  PERFETTO_ALWAYS_INLINE void pop_back() {
    PERFETTO_DCHECK(IsSinglePage());
    PERFETTO_DCHECK(tail_byte_size_ >= sizeof(T));
    tail_byte_size_ -= sizeof(T);
  }

  // Random-access lookup. O(1) when single-page; O(log n_pages) otherwise.
  PERFETTO_ALWAYS_INLINE T& operator[](uint64_t i) {
    PERFETTO_DCHECK(i < size());
    if (PERFETTO_LIKELY(frozen_byte_total_ == 0)) {
      return TypedAt(tail_.data(), i);
    }
    return AtSlow(i);
  }

  PERFETTO_ALWAYS_INLINE const T& operator[](uint64_t i) const {
    PERFETTO_DCHECK(i < size());
    if (PERFETTO_LIKELY(frozen_byte_total_ == 0)) {
      return TypedAt(tail_.data(), i);
    }
    return const_cast<PagedVector*>(this)->AtSlow(i);
  }

  // Clears the vector, resetting size to zero but retaining the tail
  // allocation. Frozen pages are dropped, including releasing any evicted
  // bytes from the attached PageStore.
  void clear() {
    if (page_store_ != nullptr) {
      for (auto h : frozen_handles_) {
        if (h != PageStore::kInvalidHandle) {
          page_store_->Release(h);
        }
      }
    }
    // Tally bytes that will be freed before we drop the slabs, so the
    // budget tracker stays consistent.
    uint64_t freed_resident = 0;
    if (page_store_ != nullptr) {
      for (const auto& slab : frozen_pages_) {
        freed_resident += slab.size();
      }
    }
    frozen_pages_.clear();
    frozen_handles_.clear();
    frozen_page_endpoints_.clear();
    frozen_byte_total_ = 0;
    tail_byte_size_ = 0;
    if (page_store_ != nullptr && freed_resident > 0) {
      page_store_->OnResidentDelta(
          -static_cast<int64_t>(freed_resident));
    }
  }

  // Resizes the vector to the specified size (in T-units). Only supported
  // in single-page mode. If growing, new elements are uninitialized.
  void resize(uint64_t new_size) {
    PERFETTO_DCHECK(IsSinglePage());
    const uint64_t new_byte_size = new_size * sizeof(T);
    if (new_byte_size > tail_.size()) {
      Slab<uint8_t> new_slab = AllocStorage(RoundUpBytes(new_byte_size));
      if (tail_byte_size_ > 0) {
        std::memcpy(new_slab.data(), tail_.data(), tail_byte_size_);
      }
      tail_ = std::move(new_slab);
    }
    tail_byte_size_ = new_byte_size;
  }

  // Shrinks the tail to size. Only supported in single-page mode.
  void shrink_to_fit() {
    PERFETTO_DCHECK(IsSinglePage());
    if (tail_byte_size_ == 0) {
      tail_ = AllocStorage(0);
    } else {
      Slab<uint8_t> new_slab = AllocStorage(RoundUpBytes(tail_byte_size_));
      std::memcpy(new_slab.data(), tail_.data(), tail_byte_size_);
      tail_ = std::move(new_slab);
    }
  }

  // Contiguous data accessors. Only valid in single-page mode.
  PERFETTO_ALWAYS_INLINE T* data() {
    PERFETTO_DCHECK(IsSinglePage());
    return reinterpret_cast<T*>(tail_.data());
  }
  PERFETTO_ALWAYS_INLINE const T* data() const {
    PERFETTO_DCHECK(IsSinglePage());
    return reinterpret_cast<const T*>(tail_.data());
  }

  PERFETTO_ALWAYS_INLINE uint64_t size() const {
    return (frozen_byte_total_ + tail_byte_size_) / sizeof(T);
  }
  PERFETTO_ALWAYS_INLINE bool empty() const { return size() == 0; }

  // Iteration. Only valid in single-page mode.
  PERFETTO_ALWAYS_INLINE T* begin() {
    PERFETTO_DCHECK(IsSinglePage());
    return reinterpret_cast<T*>(tail_.data());
  }
  PERFETTO_ALWAYS_INLINE T* end() {
    PERFETTO_DCHECK(IsSinglePage());
    return reinterpret_cast<T*>(tail_.data() + tail_byte_size_);
  }
  PERFETTO_ALWAYS_INLINE const T* begin() const {
    PERFETTO_DCHECK(IsSinglePage());
    return reinterpret_cast<const T*>(tail_.data());
  }
  PERFETTO_ALWAYS_INLINE const T* end() const {
    PERFETTO_DCHECK(IsSinglePage());
    return reinterpret_cast<const T*>(tail_.data() + tail_byte_size_);
  }

  PERFETTO_ALWAYS_INLINE const T& back() const {
    PERFETTO_DCHECK(!empty());
    if (tail_byte_size_ > 0) {
      return TypedAt(tail_.data(),
                     tail_byte_size_ / sizeof(T) - 1);
    }
    const Slab<uint8_t>& last = frozen_pages_.back();
    return TypedAt(last.data(), page_bytes_ / sizeof(T) - 1);
  }
  PERFETTO_ALWAYS_INLINE T& back() {
    PERFETTO_DCHECK(!empty());
    if (tail_byte_size_ > 0) {
      return TypedAt(tail_.data(), tail_byte_size_ / sizeof(T) - 1);
    }
    Slab<uint8_t>& last = frozen_pages_.back();
    return TypedAt(last.data(), page_bytes_ / sizeof(T) - 1);
  }

  // Total tail capacity in T-units (does not include frozen pages).
  PERFETTO_ALWAYS_INLINE uint64_t capacity() const {
    return tail_.size() / sizeof(T);
  }

  // ---------------------------------------------------------------------
  // Multi-page API.
  // ---------------------------------------------------------------------

  // Number of pages, including the tail (1 + num_frozen_pages).
  PERFETTO_ALWAYS_INLINE uint32_t num_pages() const {
    return static_cast<uint32_t>(frozen_pages_.size()) + 1u;
  }

  // True iff the vector is logically a single contiguous range — no frozen
  // pages. Hot paths can branch on this.
  PERFETTO_ALWAYS_INLINE bool IsSinglePage() const {
    return frozen_pages_.empty();
  }

  // Returns a (data, length, base_index) tuple for the page at index `idx`.
  // The page indexed `num_pages() - 1` is the tail (mutable, may grow).
  // Length and base_index are in T-units.
  struct PageView {
    T* data;
    uint64_t length;
    uint64_t base_index;
  };

  // Returns a view of page `idx`. The page MUST be currently resident; if it
  // may have been evicted, call EnsureResident(idx) first. The mutable tail
  // (idx == num_pages() - 1) is always resident.
  PageView page(uint32_t idx) {
    PERFETTO_DCHECK(idx < num_pages());
    const uint64_t page_t_units = page_bytes_ / sizeof(T);
    if (idx < frozen_pages_.size()) {
      PERFETTO_DCHECK(frozen_pages_[idx].size() > 0);
      return PageView{reinterpret_cast<T*>(frozen_pages_[idx].data()),
                      page_t_units,
                      idx * page_t_units};
    }
    return PageView{reinterpret_cast<T*>(tail_.data()),
                    tail_byte_size_ / sizeof(T),
                    frozen_byte_total_ / sizeof(T)};
  }

  // Returns the logical [base, base+size) range of page `idx` in T-units.
  // Does NOT require the page to be resident — useful for callers that need
  // to decide whether to bother loading a page (e.g. range-overlap checks
  // in WalkPages).
  PERFETTO_ALWAYS_INLINE uint64_t page_base(uint32_t idx) const {
    PERFETTO_DCHECK(idx < num_pages());
    return idx * (page_bytes_ / sizeof(T));
  }

  PERFETTO_ALWAYS_INLINE uint64_t page_size(uint32_t idx) const {
    PERFETTO_DCHECK(idx < num_pages());
    if (idx < frozen_pages_.size()) {
      return page_bytes_ / sizeof(T);
    }
    return tail_byte_size_ / sizeof(T);
  }

  // Per-page endpoints: first and last element bytes captured at Seal time.
  //
  // For a sorted column, page_first_value(i) <= page_last_value(i) and
  // page_last_value(i) <= page_first_value(i+1), so a binary-search-style
  // SortedFilter can skip a frozen page entirely when the search target
  // falls outside [first, last] without restoring the page.
  //
  // Layout (each side is `kStorageAlignment` = 8 bytes):
  //   `first` holds the first 8 bytes of the page, indexed naturally.
  //   `last`  holds the last 8 bytes of the page, end-aligned — i.e. the
  //           last element's bytes sit at `last[8 - sizeof(U)..8]`.
  //
  // The caller picks the type U (must be trivially copyable, sizeof(U) ≤ 8).
  // Both typed PagedVector<T> and the reinterpret_cast pattern (used by
  // tree storage's PagedVector<uint8_t>) work without further plumbing.
  //
  // Only valid for frozen pages (idx < num_pages() - 1). Asking for the
  // tail is a programming error; the tail's last element is `back()`.
  template <typename U>
  PERFETTO_ALWAYS_INLINE U page_first_value(uint32_t idx) const {
    static_assert(sizeof(U) <= kStorageAlignment,
                  "page_first_value: U too large for PagedVector layout");
    PERFETTO_DCHECK(idx < frozen_page_endpoints_.size());
    U v;
    std::memcpy(&v, frozen_page_endpoints_[idx].first, sizeof(U));
    return v;
  }
  template <typename U>
  PERFETTO_ALWAYS_INLINE U page_last_value(uint32_t idx) const {
    static_assert(sizeof(U) <= kStorageAlignment,
                  "page_last_value: U too large for PagedVector layout");
    PERFETTO_DCHECK(idx < frozen_page_endpoints_.size());
    U v;
    std::memcpy(
        &v,
        frozen_page_endpoints_[idx].last + (kStorageAlignment - sizeof(U)),
        sizeof(U));
    return v;
  }

  // Seal the current tail, freezing it into a new page and starting a fresh
  // empty tail. Private — invoked only by `push_back`'s auto-seal trigger.
  // Sealing is not user-callable: pages are sized by `kPageBytes` (or the
  // test override) and the only way to create a new page is to fill the
  // tail past that threshold.
 private:
  void Seal() {
    if (tail_byte_size_ == 0) {
      return;
    }
    // Capture per-page endpoints before sealing, while the tail bytes are
    // still resident. `first` = first 8 bytes of the page (low-aligned),
    // `last` = last 8 bytes of the page (high-aligned, so the last element
    // bytes sit flush with the high end). See page_first_value /
    // page_last_value for the read-side contract.
    PageEndpoints endpoints{};
    const size_t copy_bytes =
        std::min<size_t>(kStorageAlignment, tail_byte_size_);
    std::memcpy(endpoints.first, tail_.data(), copy_bytes);
    std::memcpy(endpoints.last + (kStorageAlignment - copy_bytes),
                tail_.data() + tail_byte_size_ - copy_bytes, copy_bytes);
    frozen_page_endpoints_.push_back(endpoints);
    // Auto-seal only fires when tail_byte_size_ reaches page_bytes_, which
    // is enforced to be a multiple of sizeof(T). So every frozen page is
    // exactly page_bytes_ bytes — uniform, indexable in O(1).
    PERFETTO_DCHECK(tail_byte_size_ == page_bytes_);
    // shrink-to-fit the tail before sealing.
    Slab<uint8_t> sealed = AllocStorage(RoundUpBytes(tail_byte_size_));
    std::memcpy(sealed.data(), tail_.data(), tail_byte_size_);
    uint64_t sealed_capacity = sealed.size();
    uint64_t old_tail_capacity = tail_.size();
    frozen_byte_total_ += tail_byte_size_;
    frozen_pages_.push_back(std::move(sealed));
    frozen_handles_.push_back(PageStore::kInvalidHandle);
    tail_ = AllocStorage(0);
    tail_byte_size_ = 0;
    // Resident bytes delta: we added a sealed slab (`sealed_capacity` bytes)
    // and replaced the old tail (size `old_tail_capacity`) with an empty
    // tail. Net = sealed - old_tail.
    if (page_store_ != nullptr) {
      int64_t delta = static_cast<int64_t>(sealed_capacity) -
                      static_cast<int64_t>(old_tail_capacity);
      page_store_->OnResidentDelta(delta);
    }
  }

 public:
  // ---------------------------------------------------------------------
  // Eviction. Lets a frozen page be flushed out of the in-memory slab into
  // a backing PageStore. Reads after eviction transparently restore.
  // Only frozen pages can be evicted — the mutable tail is always resident.
  // ---------------------------------------------------------------------

  PageStore* page_store() const { return page_store_; }

  // Returns true if the frozen page at `idx` has bytes resident in RAM.
  // The tail is always resident.
  bool IsPageResident(uint32_t idx) const {
    if (idx >= frozen_pages_.size()) {
      return true;
    }
    return frozen_pages_[idx].size() > 0;
  }

  // Evicts the bytes of frozen page `idx` to the attached PageStore and drops
  // the in-memory slab. The page metadata stays so structural queries still
  // work.
  void EvictPage(uint32_t idx) {
    PERFETTO_CHECK(page_store_ != nullptr);
    PERFETTO_CHECK(idx < frozen_pages_.size());
    if (frozen_pages_[idx].size() == 0) {
      return;  // Already evicted.
    }
    auto& slab = frozen_pages_[idx];
    PERFETTO_CHECK(frozen_handles_[idx] == PageStore::kInvalidHandle);
    uint64_t freed_bytes = slab.size();
    frozen_handles_[idx] = page_store_->Store(slab.data(), page_bytes_);
    slab = Slab<uint8_t>{};  // Drop the in-memory slab.
    if (page_store_ != nullptr) {
      page_store_->OnResidentDelta(-static_cast<int64_t>(freed_bytes));
    }
  }

  // Ensures frozen page `idx` is resident in RAM. If currently evicted,
  // allocates a fresh slab and reads back from the PageStore.
  void EnsureResident(uint32_t idx) {
    PERFETTO_DCHECK(idx <= frozen_pages_.size());
    if (idx >= frozen_pages_.size()) {
      return;  // Tail is always resident.
    }
    if (frozen_pages_[idx].size() > 0) {
      return;  // Already resident.
    }
    PERFETTO_CHECK(page_store_ != nullptr);
    PERFETTO_CHECK(frozen_handles_[idx] != PageStore::kInvalidHandle);
    Slab<uint8_t> restored = AllocStorage(RoundUpBytes(page_bytes_));
    uint64_t restored_capacity = restored.size();
    page_store_->Load(frozen_handles_[idx], restored.data(),
                      static_cast<size_t>(page_bytes_));
    frozen_pages_[idx] = std::move(restored);
    page_store_->Release(frozen_handles_[idx]);
    frozen_handles_[idx] = PageStore::kInvalidHandle;
    // Use the no-evict path: restoring a page exists specifically so the
    // caller can read it; if budget enforcement evicted it back out, the
    // caller would dereference a dropped slab. The next allocation / seal
    // will trim residency back under budget.
    page_store_->RecordResidentDelta(
        static_cast<int64_t>(restored_capacity));
  }

  // Test-only knob to override the auto-seal threshold. Production code
  // must NOT call this — pages always seal at `kPageBytes`. Tests use this
  // to verify multi-page behaviour without having to push 4 MiB of data.
  // The argument is in BYTES and must be a multiple of `sizeof(T)` (so
  // every frozen page ends up exactly `bytes` bytes — the
  // `byte_offset / page_bytes_` page lookup in AtSlow stays correct).
  // Must be called BEFORE any data is pushed: pages need to be uniform.
  void SetPageBytesForTesting(uint64_t bytes) {
    PERFETTO_DCHECK(bytes % sizeof(T) == 0);
    PERFETTO_DCHECK(frozen_pages_.empty());
    PERFETTO_DCHECK(tail_byte_size_ == 0);
    page_bytes_ = bytes;
  }

  // Global test-only knob: when set non-zero, every newly constructed
  // PagedVector starts with this page size instead of `kPageBytes`. Useful
  // for forcing every column in a test fixture into multi-page mode so
  // handlers are exercised against the multi-page codepath. Set to 0 to
  // disable. Not thread-safe; use only from single-threaded test setup.
  static uint64_t& GlobalPageBytesOverrideForTesting() {
    static uint64_t v = 0;
    return v;
  }

 private:
  // Constructor used by static factory methods. `byte_capacity` is the
  // capacity of the tail slab in bytes; `byte_size` is the initial logical
  // size in bytes (not necessarily a multiple of sizeof(T)). `store` must
  // be non-null.
  explicit PagedVector(uint64_t byte_capacity,
                       uint64_t byte_size,
                       PageStore* store)
      : tail_(AllocStorage(byte_capacity)), tail_byte_size_(byte_size) {
    PERFETTO_CHECK(store != nullptr);
    AttachPageStoreInternal(store);
  }

  // Wires this PagedVector to `store`: registers as an evictee and reports
  // currently-resident bytes. Used from constructors only. `store` is
  // guaranteed non-null by callers.
  void AttachPageStoreInternal(PageStore* store) {
    PERFETTO_DCHECK(page_store_ == nullptr);
    page_store_ = store;
    evictee_handle_ = store->Register(&PagedVector::EvictThunk, this);
    uint64_t resident = ComputeResidentBytes();
    if (resident > 0) {
      store->OnResidentDelta(static_cast<int64_t>(resident));
    }
  }

  static PERFETTO_ALWAYS_INLINE uint64_t RoundUpBytes(uint64_t bytes) {
    return base::AlignUp(bytes, kCapacityMultipleBytes);
  }

  static PERFETTO_ALWAYS_INLINE Slab<uint8_t> AllocStorage(uint64_t bytes) {
    return Slab<uint8_t>::Alloc(bytes, kStorageAlignment);
  }

  // Reinterprets `byte_ptr` as `T*` and returns element `i`. Used by all
  // typed accessors in the hot path. Compiler optimizes away the cast for
  // trivially-copyable T with appropriate alignment.
  PERFETTO_ALWAYS_INLINE static T& TypedAt(uint8_t* byte_ptr, uint64_t i) {
    return *reinterpret_cast<T*>(byte_ptr + i * sizeof(T));
  }
  PERFETTO_ALWAYS_INLINE static const T& TypedAt(const uint8_t* byte_ptr,
                                                 uint64_t i) {
    return *reinterpret_cast<const T*>(byte_ptr + i * sizeof(T));
  }

  PERFETTO_NO_INLINE void IncreaseCapacity() {
    uint64_t new_byte_capacity = std::max<uint64_t>(
        RoundUpBytes(static_cast<size_t>(static_cast<double>(tail_.size()) *
                                         kGrowthFactor)),
        kCapacityMultipleBytes);
    Slab<uint8_t> new_slab = AllocStorage(new_byte_capacity);
    if (tail_.size() > 0 && tail_byte_size_ > 0) {
      std::memcpy(new_slab.data(), tail_.data(), tail_byte_size_);
    }
    int64_t delta = static_cast<int64_t>(new_byte_capacity) -
                    static_cast<int64_t>(tail_.size());
    tail_ = std::move(new_slab);
    if (page_store_ != nullptr && delta != 0) {
      page_store_->OnResidentDelta(delta);
    }
  }

  PERFETTO_NO_INLINE T& AtSlow(uint64_t i) {
    const uint64_t byte_offset = i * sizeof(T);
    if (byte_offset >= frozen_byte_total_) {
      return TypedAt(tail_.data(),
                     (byte_offset - frozen_byte_total_) / sizeof(T));
    }
    // Frozen pages are uniformly `page_bytes_` bytes (auto-seal fires
    // exactly at the threshold), so the page index is a single division
    // — no binary search over per-page offsets needed.
    auto idx = static_cast<uint32_t>(byte_offset / page_bytes_);
    uint64_t in_page_byte_offset = byte_offset - idx * page_bytes_;
    EnsureResident(idx);
    return TypedAt(frozen_pages_[idx].data(),
                   in_page_byte_offset / sizeof(T));
  }

  // First and last sizeof(T)-bytes of a frozen page, captured at Seal time.
  // Used by SortedFilter and similar to skip pages that can't possibly
  // contain a search target without restoring the page from disk.
  // Stored as raw bytes (kStorageAlignment = 8) so the layout is identical
  // across all `PagedVector<T>` instantiations and reinterpret_cast through
  // the type-erased view (e.g. tree storage's PagedVector<uint8_t>) keeps
  // the metadata accessible.
  struct PageEndpoints {
    uint8_t first[kStorageAlignment];
    uint8_t last[kStorageAlignment];
  };

  // Frozen pages: read-only, never resized after Seal(). When evicted the
  // slab is replaced with a default-constructed (empty) Slab and the bytes
  // live in the PageStore under the matching handle. Every frozen page
  // contains exactly `page_bytes_` bytes of logical data — uniformly sized,
  // so a row's page index is `(row * sizeof(T)) / page_bytes_` (no
  // per-page offset table needed).
  std::vector<Slab<uint8_t>> frozen_pages_;
  // PageStore handle for each frozen page; kInvalidHandle iff resident.
  std::vector<PageStore::Handle> frozen_handles_;
  // First/last element bytes per frozen page (parallel to frozen_pages_).
  // Populated by Seal() so the metadata survives eviction.
  std::vector<PageEndpoints> frozen_page_endpoints_;
  // Total bytes across frozen pages.
  uint64_t frozen_byte_total_ = 0;
  // Backing store for evicted pages. Non-owning. May be null if the column
  // is never evicted.
  PageStore* page_store_ = nullptr;

  // Mutable tail. Grows on push_back; shrink_to_fit / resize / pop_back act
  // on it directly. After Seal(), a fresh empty tail is allocated.
  Slab<uint8_t> tail_;
  // Logical bytes used in the tail.
  uint64_t tail_byte_size_ = 0;
  // Auto-seal threshold in BYTES. Defaults to `kPageBytes`, or to the
  // global testing override if non-zero (for fixture-driven multi-page
  // forcing). Tests can also call `SetPageBytesForTesting` for per-instance
  // overrides.
  uint64_t page_bytes_ =
      GlobalPageBytesOverrideForTesting() != 0
          ? GlobalPageBytesOverrideForTesting()
          : kPageBytes;
  // PageStore registration. Valid iff `page_store_ != nullptr`. Layout-
  // stable across T (a std::list<>::iterator).
  PageStore::EvicteeHandle evictee_handle_{};

  // Total bytes currently resident in this PagedVector: tail allocation
  // size + sum of frozen page slab sizes (excluding evicted pages whose
  // slabs are size 0).
  uint64_t ComputeResidentBytes() const {
    uint64_t r = tail_.size();
    for (size_t i = 0; i < frozen_pages_.size(); ++i) {
      r += frozen_pages_[i].size();
    }
    return r;
  }

  // Type-erased thunk registered with the PageStore. Forwards to the typed
  // evict-oldest implementation.
  static uint64_t EvictThunk(void* ctx) {
    return static_cast<PagedVector*>(ctx)->EvictOldestResidentFrozenPage();
  }

  // Evicts the oldest currently-resident frozen page (lowest index whose
  // slab is not yet evicted). Returns the byte count freed, or 0 if there
  // is no resident frozen page.
  uint64_t EvictOldestResidentFrozenPage() {
    for (uint32_t i = 0; i < frozen_pages_.size(); ++i) {
      if (frozen_pages_[i].size() > 0) {
        uint64_t freed = frozen_pages_[i].size();
        EvictPage(i);
        return freed;
      }
    }
    return 0;
  }
};

// RAII helper: ensures a specific page is resident for the lifetime of the
// scope. Use when an instruction needs to touch one specific page (e.g. for
// a binary search probe, or a one-shot random access).
template <typename T>
class ScopedPage {
 public:
  ScopedPage(PagedVector<T>& vec, uint32_t page_idx)
      : vec_(vec), idx_(page_idx) {
    vec_.EnsureResident(idx_);
    view_ = vec_.page(idx_);
  }

  ScopedPage(const ScopedPage&) = delete;
  ScopedPage& operator=(const ScopedPage&) = delete;

  PERFETTO_ALWAYS_INLINE const T* data() const { return view_.data; }
  PERFETTO_ALWAYS_INLINE T* data() { return view_.data; }
  PERFETTO_ALWAYS_INLINE uint64_t size() const { return view_.length; }
  PERFETTO_ALWAYS_INLINE uint64_t base_index() const {
    return view_.base_index;
  }

 private:
  PagedVector<T>& vec_;
  uint32_t idx_;
  typename PagedVector<T>::PageView view_{};
};

// Walks the pages of `vec` that overlap the logical index range [begin, end),
// invoking `fn(page_view)` for each. Each page is made resident before the
// callback runs. The view's `base_index` and `length` reflect the *full*
// page; the callback is responsible for clipping to [begin, end) if needed.
//
// Single-page hot path: when the column is single-page, this collapses to a
// single callback invocation with no per-element overhead.
template <typename T, typename Fn>
PERFETTO_ALWAYS_INLINE void WalkPages(PagedVector<T>& vec,
                                      uint64_t begin,
                                      uint64_t end,
                                      Fn&& fn) {
  PERFETTO_DCHECK(begin <= end);
  PERFETTO_DCHECK(end <= vec.size());
  if (begin == end) {
    return;
  }
  if (PERFETTO_LIKELY(vec.IsSinglePage())) {
    auto view = vec.page(0);
    fn(view);
    return;
  }
  uint32_t n_pages = vec.num_pages();
  for (uint32_t i = 0; i < n_pages; ++i) {
    // Check overlap *before* materialising the page so we don't restore
    // pages outside [begin, end) just to skip them.
    uint64_t page_b = vec.page_base(i);
    uint64_t page_e = page_b + vec.page_size(i);
    if (page_e <= begin) {
      continue;
    }
    if (page_b >= end) {
      break;
    }
    vec.EnsureResident(i);
    fn(vec.page(i));
  }
}

}  // namespace perfetto::trace_processor::core

#endif  // SRC_TRACE_PROCESSOR_CORE_UTIL_PAGED_VECTOR_H_
