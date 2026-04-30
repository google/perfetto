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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PERFETTO_SQL_DATABASE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PERFETTO_SQL_DATABASE_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/perfetto_sql/parser/function_util.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/util/sql_argument.h"

namespace perfetto::trace_processor {

// Additive-only versioned pool. Connections diff against the latest
// version at the top of each `Execute`. The atomic version is bumped
// after the entry lands in the vector so a reader observing
// `LatestVersion() >= v` is guaranteed to see the entry once it
// acquires the mutex.
template <typename Entry>
class VersionedPool {
 public:
  struct Snapshot {
    std::vector<Entry> entries;
    uint64_t latest_version = 0;
  };

  uint64_t Append(Entry entry) {
    std::lock_guard<std::mutex> g(mu_);
    pool_.push_back(std::move(entry));
    uint64_t v = pool_.size();
    version_.store(v, std::memory_order_release);
    return v;
  }

  // Cheap fast-path: empty `entries` and `latest_version == since` if
  // the caller is already up-to-date — no lock taken.
  Snapshot SnapshotSince(uint64_t since) const {
    if (version_.load(std::memory_order_acquire) <= since) {
      return {{}, since};
    }
    std::lock_guard<std::mutex> g(mu_);
    Snapshot s;
    s.latest_version = pool_.size();
    if (since >= s.latest_version) {
      return s;
    }
    s.entries.assign(pool_.begin() + static_cast<ptrdiff_t>(since),
                     pool_.end());
    return s;
  }

  uint64_t LatestVersion() const {
    return version_.load(std::memory_order_acquire);
  }

  // Caller must guarantee no reader is observing the pool (e.g. via
  // the `non_default_connection_count_ == 0` precondition in
  // `RestoreInitialTables`).
  void Reset() {
    std::lock_guard<std::mutex> g(mu_);
    pool_.clear();
    version_.store(0, std::memory_order_release);
  }

 private:
  mutable std::mutex mu_;
  std::vector<Entry> pool_;
  std::atomic<uint64_t> version_{0};
};

// Cross-connection state shared by every PerfettoSqlEngine attached
// to the same TraceProcessorImpl. Owned by TraceProcessorImpl and
// passed by pointer into each connection at construction time.
//
// Holds:
//   - the vtab-state map (writer publishes on `OnCommit`; reader
//     looks up during cold xConnect),
//   - additive-only function and package pools that each connection
//     diffs against at the start of `Execute`,
//   - a single mutex+condvar coordinating `INCLUDE PERFETTO MODULE`
//     across connections (see `TryClaimInclude`).
class PerfettoSqlDatabase {
 public:
  // ===== Function pool =====
  // Additive-only registry of dynamic PerfettoSQL functions. Each
  // connection tracks `last_synced_function_version_` and diffs at the
  // top of `Execute`, registering missing entries on its own
  // `sqlite3*`. Replacement (`CREATE OR REPLACE`) appends a fresh
  // entry with `replace=true`; SQLite's normal function re-
  // registration semantics handle the in-engine overwrite. Scope of
  // v1: scalar dynamic functions only — TABLE-returning functions and
  // static built-ins are not yet replicated.
  struct FunctionPoolEntry {
    FunctionPoolEntry(bool _replace,
                      FunctionPrototype _prototype,
                      sql_argument::Type _return_type,
                      SqlSource _sql)
        : replace(_replace),
          prototype(std::move(_prototype)),
          return_type(_return_type),
          sql(std::move(_sql)) {}
    bool replace = false;
    FunctionPrototype prototype;
    sql_argument::Type return_type = sql_argument::Type::kLong;
    SqlSource sql;
  };
  VersionedPool<FunctionPoolEntry> functions;

  // ===== Package pool =====
  // Additive-only registry of `RegisterSqlPackage` calls. Stores raw
  // (module-name, sql) pairs (rather than the move-only
  // `RegisteredPackage`) so multiple readers can rebuild their own
  // engine-local copies from the same payload.
  struct PackagePoolEntry {
    using Modules = std::vector<std::pair<std::string, std::string>>;
    PackagePoolEntry(std::string _name,
                     bool _allow_replace,
                     std::shared_ptr<const Modules> _modules)
        : name(std::move(_name)),
          allow_replace(_allow_replace),
          modules(std::move(_modules)) {}
    std::string name;
    bool allow_replace = false;
    std::shared_ptr<const Modules> modules;
  };
  VersionedPool<PackagePoolEntry> packages;

  PerfettoSqlDatabase();
  ~PerfettoSqlDatabase();

  PerfettoSqlDatabase(const PerfettoSqlDatabase&) = delete;
  PerfettoSqlDatabase& operator=(const PerfettoSqlDatabase&) = delete;
  PerfettoSqlDatabase(PerfettoSqlDatabase&&) = delete;
  PerfettoSqlDatabase& operator=(PerfettoSqlDatabase&&) = delete;

  // Cross-connection vtab-state map keyed by `(module_name, vtab_name)`.
  // Stored as `shared_ptr<void>` so different vtab modules can store
  // different state types; producers and consumers must agree on the
  // type and `static_pointer_cast` at the call site.
  void PublishVtabState(const std::string& module_name,
                        const std::string& vtab_name,
                        std::shared_ptr<void> state);
  void RemoveVtabState(const std::string& module_name,
                       const std::string& vtab_name);
  std::shared_ptr<void> LookupVtabState(const std::string& module_name,
                                        const std::string& vtab_name) const;

  // RAII guard returned by `TryClaimInclude`. The caller owns the
  // in-flight slot for the module key and must call `Release(success)`
  // (or destruct, treated as failure) so other waiters are unblocked.
  class IncludeClaim {
   public:
    IncludeClaim() = default;
    IncludeClaim(PerfettoSqlDatabase* db, std::string key)
        : db_(db), key_(std::move(key)) {}
    IncludeClaim(IncludeClaim&& o) noexcept
        : db_(std::exchange(o.db_, nullptr)), key_(std::move(o.key_)) {}
    IncludeClaim& operator=(IncludeClaim&& o) noexcept {
      if (this != &o) {
        Reset(/*success=*/false);
        db_ = std::exchange(o.db_, nullptr);
        key_ = std::move(o.key_);
      }
      return *this;
    }
    ~IncludeClaim() { Reset(/*success=*/false); }
    IncludeClaim(const IncludeClaim&) = delete;
    IncludeClaim& operator=(const IncludeClaim&) = delete;
    void Release(bool success) { Reset(success); }

   private:
    void Reset(bool success);
    PerfettoSqlDatabase* db_ = nullptr;
    std::string key_;
  };
  struct IncludeClaimResult {
    IncludeClaim claim;       // valid iff `!already_included`
    bool already_included = false;
  };

  // Claims the include slot for `module_name`. Blocks until no other
  // thread is mid-import for the same key. Self-recursive includes
  // (same thread already importing this key) deadlock by design — the
  // caller must short-circuit via the per-engine
  // `RegisteredPackage::ModuleFile::included` flag *before* calling.
  IncludeClaimResult TryClaimInclude(const std::string& module_name);

  // Reset the cross-connection "already included" set. Called from
  // `RestoreInitialTables` (which already requires zero secondary
  // connections, so no in-flight claims).
  void ResetIncludedModules();

 private:
  friend class IncludeClaim;
  void ReleaseClaim(const std::string& key, bool success);

  static std::string MakeVtabKey(const std::string& module_name,
                                 const std::string& vtab_name);

  // Single mutex + condvar for the include-claim machinery + the
  // already-included set. Waiters block on `cv_` while a peer thread
  // holds an in-progress slot for the same key.
  mutable std::mutex include_mu_;
  std::condition_variable include_cv_;
  std::unordered_set<std::string> in_progress_;
  std::unordered_set<std::string> included_modules_;

  mutable std::mutex vtab_state_mutex_;
  base::FlatHashMap<std::string, std::shared_ptr<void>> vtab_state_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PERFETTO_SQL_DATABASE_H_
