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

#ifndef SRC_TRACE_PROCESSOR_CORE_UTIL_PAGE_STORE_H_
#define SRC_TRACE_PROCESSOR_CORE_UTIL_PAGE_STORE_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"

namespace perfetto::trace_processor::core {

// PageStorage: backing-tier strategy. Implementations move slabs of bytes
// between RAM and a secondary tier — an in-memory buffer for tests, an
// on-disk temp file in native mode, or OPFS in browser. Handles are
// integers chosen by the implementation; zero is reserved for "not stored".
//
// PageStorage is the ONLY virtual interface in this file. It is plugged
// into a `PageStore` (concrete) which owns it and adds budget tracking
// and a participant registry on top.
class PageStorage {
 public:
  using Handle = uint64_t;
  static constexpr Handle kInvalidHandle = 0;

  PageStorage() = default;
  virtual ~PageStorage() = default;
  PageStorage(const PageStorage&) = delete;
  PageStorage& operator=(const PageStorage&) = delete;

  // Allocates a new region of `size_bytes` and copies `data` into it.
  // Returns the handle that can later be used to retrieve the bytes. Must
  // not return kInvalidHandle.
  virtual Handle Store(const void* data, size_t size_bytes) = 0;

  // Reads `size_bytes` from the previously-stored region into `out`.
  virtual void Load(Handle h, void* out, size_t size_bytes) = 0;

  // Releases the storage for a handle. The handle must not be used after.
  virtual void Release(Handle h) = 0;
};

// In-memory PageStorage: keeps evicted pages in heap-resident byte vectors.
// Useful for tests and as a building block; does NOT actually free RAM. For
// real RAM relief use TempFilePageStorage (or an OPFS variant in browser).
class InMemoryPageStorage final : public PageStorage {
 public:
  Handle Store(const void* data, size_t size_bytes) override {
    Handle h = next_handle_++;
    auto& buf = entries_[h];
    buf.assign(static_cast<const uint8_t*>(data),
               static_cast<const uint8_t*>(data) + size_bytes);
    return h;
  }
  void Load(Handle h, void* out, size_t size_bytes) override {
    auto it = entries_.find(h);
    PERFETTO_CHECK(it != entries_.end());
    PERFETTO_CHECK(it->second.size() == size_bytes);
    std::memcpy(out, it->second.data(), size_bytes);
  }
  void Release(Handle h) override { entries_.erase(h); }

  size_t entry_count() const { return entries_.size(); }
  size_t total_bytes() const {
    size_t total = 0;
    for (const auto& kv : entries_) {
      total += kv.second.size();
    }
    return total;
  }

 private:
  Handle next_handle_ = 1;
  std::unordered_map<Handle, std::vector<uint8_t>> entries_;
};

// Disk-backed PageStorage: spills evicted pages to a single temp file. Each
// stored region is appended; freed regions are simply marked as such (no
// hole reuse — appropriate for the typical "build column once, query many
// times" workload). Reads use absolute file offsets.
//
// On construction a temp file is opened in the OS's default tmpdir.
class TempFilePageStorage final : public PageStorage {
 public:
  TempFilePageStorage() {
    file_ = std::tmpfile();
    PERFETTO_CHECK(file_ != nullptr);
  }
  ~TempFilePageStorage() override {
    if (file_) {
      std::fclose(file_);
    }
  }

  Handle Store(const void* data, size_t size_bytes) override {
    int rc = std::fseek(file_, 0, SEEK_END);
    PERFETTO_CHECK(rc == 0);
    long off = std::ftell(file_);
    PERFETTO_CHECK(off >= 0);
    size_t written = std::fwrite(data, 1, size_bytes, file_);
    PERFETTO_CHECK(written == size_bytes);
    Handle h = next_handle_++;
    entries_.emplace(h, Entry{static_cast<uint64_t>(off), size_bytes});
    return h;
  }
  void Load(Handle h, void* out, size_t size_bytes) override {
    auto it = entries_.find(h);
    PERFETTO_CHECK(it != entries_.end());
    PERFETTO_CHECK(it->second.size == size_bytes);
    int rc = std::fseek(file_, static_cast<long>(it->second.offset), SEEK_SET);
    PERFETTO_CHECK(rc == 0);
    size_t read = std::fread(out, 1, size_bytes, file_);
    PERFETTO_CHECK(read == size_bytes);
  }
  void Release(Handle h) override { entries_.erase(h); }

 private:
  struct Entry {
    uint64_t offset;
    size_t size;
  };
  std::FILE* file_ = nullptr;
  Handle next_handle_ = 1;
  std::unordered_map<Handle, Entry> entries_;
};

// Concrete page registry. Combines two responsibilities:
//
//   1. Backing tier: forwards Store/Load/Release to its `PageStorage`.
//
//   2. Memory budget: tracks resident bytes across every PagedVector
//      registered with this store. When the total exceeds the budget,
//      walks participants round-robin asking each to evict its oldest
//      resident frozen page.
//
// PagedVector calls `OnResidentDelta` whenever its resident byte count
// changes (slab allocate / seal / evict / restore); the store handles the
// eviction loop. There is one PageStore per "memory region of interest" —
// typically one per trace processor session.
//
// Lifetime: PageStore MUST outlive every PagedVector that registered with
// it. Participants Unregister in their destructor; if the store dies first
// the participant will reach into a freed list. The DCHECK in `~PageStore`
// catches the violation.
//
// Thread-safety: not thread-safe. Trace processor's parse and query paths
// are single-threaded.
class PageStore {
 public:
  using Handle = PageStorage::Handle;
  static constexpr Handle kInvalidHandle = PageStorage::kInvalidHandle;

  // Default resident budget: 10 GiB. Picked as the headroom point above
  // which a typical trace processor session is at risk of OOMing the host.
  static constexpr uint64_t kDefaultBudgetBytes = 10ull * 1024 * 1024 * 1024;

  // Called when the store asks a participant to free RAM. Should evict the
  // participant's oldest resident page and return the bytes freed (0 if
  // nothing was resident).
  using EvictFn = uint64_t (*)(void* ctx);

 private:
  struct Evictee {
    EvictFn evict_fn;
    void* ctx;
  };

 public:
  using EvicteeHandle = std::list<Evictee>::iterator;

  explicit PageStore(std::unique_ptr<PageStorage> storage)
      : storage_(std::move(storage)), next_evictee_(evictees_.end()) {
    PERFETTO_CHECK(storage_ != nullptr);
  }
  ~PageStore() { PERFETTO_DCHECK(evictees_.empty()); }
  PageStore(const PageStore&) = delete;
  PageStore& operator=(const PageStore&) = delete;

  // ---- Backing tier (forwarded to the PageStorage) ----------------------

  Handle Store(const void* data, size_t size_bytes) {
    return storage_->Store(data, size_bytes);
  }
  void Load(Handle h, void* out, size_t size_bytes) {
    storage_->Load(h, out, size_bytes);
  }
  void Release(Handle h) { storage_->Release(h); }

  // Borrowed accessor for the backing storage. Useful for tests that want
  // to query implementation-specific state (e.g.
  // `InMemoryPageStorage::entry_count`). Non-owning.
  PageStorage* storage() const { return storage_.get(); }

  // Process-wide PageStore for tests that don't need a per-fixture store.
  // Backed by an in-memory storage; pages "evicted" stay in RAM but the
  // budget machinery still works. PRODUCTION CODE MUST NOT USE THIS —
  // production columns belong to a session-level store on
  // TraceStorage. The singleton exists purely so test helpers can default
  // to a working store without each callsite plumbing one in.
  static PageStore* TestingSingleton() {
    static PageStore* instance = new PageStore(
        std::make_unique<InMemoryPageStorage>());
    return instance;
  }

  // ---- Budget management ------------------------------------------------

  uint64_t budget_bytes() const { return budget_; }
  void SetBudgetBytes(uint64_t b) {
    budget_ = b;
    if (resident_ > budget_) {
      EvictUntilUnderBudget();
    }
  }
  uint64_t resident_bytes() const { return resident_; }

  // Registers a participant in the budget. Returns a stable handle that
  // the caller uses to RebindCtx on move and Unregister on destruction.
  EvicteeHandle Register(EvictFn fn, void* ctx) {
    evictees_.push_back(Evictee{fn, ctx});
    return std::prev(evictees_.end());
  }
  void RebindCtx(EvicteeHandle h, void* new_ctx) { h->ctx = new_ctx; }
  void Unregister(EvicteeHandle h) {
    if (next_evictee_ == h) {
      ++next_evictee_;
    }
    evictees_.erase(h);
  }

  // Called by participants whenever their resident byte count changes
  // because of an action that's safe to react to with eviction (allocate /
  // grow tail / seal a page). Positive = more resident; negative = freed.
  // Triggers eviction if the updated total is over budget.
  //
  // NOTE: do NOT call this from a *restore* path (EnsureResident). Restoring
  // a page is a request to make that page accessible RIGHT NOW; evicting it
  // again before the caller dereferences would either crash or livelock. Use
  // `RecordResidentDelta` for those cases — it updates the counter without
  // evicting. Going temporarily over budget during a scan is acceptable;
  // eviction will catch up at the next allocation / seal.
  void OnResidentDelta(int64_t delta) {
    RecordResidentDelta(delta);
    if (delta > 0 && resident_ > budget_) {
      EvictUntilUnderBudget();
    }
  }

  // Counter-only update: adjusts `resident_bytes` without ever triggering
  // eviction. Use from restore paths (EnsureResident) where evicting the
  // page that was just made resident would defeat the point of restoring it.
  void RecordResidentDelta(int64_t delta) {
    if (delta >= 0) {
      resident_ += static_cast<uint64_t>(delta);
    } else {
      const uint64_t abs_delta = static_cast<uint64_t>(-delta);
      PERFETTO_DCHECK(abs_delta <= resident_);
      resident_ -= abs_delta;
    }
  }

 private:
  // Walks attached participants in round-robin order, asking each to evict
  // its oldest resident page until either the budget is met or no
  // participant has a resident page left.
  void EvictUntilUnderBudget() {
    if (evictees_.empty()) {
      return;
    }
    constexpr int kMaxPasses = 4;
    for (int pass = 0; pass < kMaxPasses && resident_ > budget_; ++pass) {
      bool any_evicted = false;
      // One pass = ask each participant once, starting from `next_evictee_`.
      // Using a count-based loop so the termination doesn't depend on the
      // iterator landing exactly on the start position (which a wrap-around
      // would skip past).
      size_t n = evictees_.size();
      for (size_t i = 0; i < n; ++i) {
        if (next_evictee_ == evictees_.end()) {
          next_evictee_ = evictees_.begin();
        }
        uint64_t freed = next_evictee_->evict_fn(next_evictee_->ctx);
        if (freed > 0) {
          any_evicted = true;
        }
        ++next_evictee_;
        if (resident_ <= budget_) {
          return;
        }
      }
      if (!any_evicted) {
        return;
      }
    }
  }

  std::unique_ptr<PageStorage> storage_;
  uint64_t budget_ = kDefaultBudgetBytes;
  uint64_t resident_ = 0;
  std::list<Evictee> evictees_;
  EvicteeHandle next_evictee_;
};

}  // namespace perfetto::trace_processor::core

#endif  // SRC_TRACE_PROCESSOR_CORE_UTIL_PAGE_STORE_H_
