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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_GLOBAL_STAGING_AREA_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_GLOBAL_STAGING_AREA_H_

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

// Cross-connection state shared by every PerfettoSqlEngine attached to the
// same TraceProcessorImpl. Owned by TraceProcessorImpl and passed by
// pointer into each connection at construction time.
//
// Holds:
//   - the vtab-state map (writer publishes on `OnCommit`; reader looks up
//     during cold xConnect),
//   - additive-only function and package pools that each connection diffs
//     against at the start of `Execute`,
//   - per-module include locks that serialise concurrent
//     `INCLUDE PERFETTO MODULE` invocations against the same module name,
//   - cross-connection "module already included" tracking so re-includes
//     short-circuit before re-running the module body on shared `main`.
class PerfettoSqlDatabase {
 public:
  PerfettoSqlDatabase();
  ~PerfettoSqlDatabase();

  PerfettoSqlDatabase(const PerfettoSqlDatabase&) = delete;
  PerfettoSqlDatabase& operator=(const PerfettoSqlDatabase&) = delete;

  PerfettoSqlDatabase(PerfettoSqlDatabase&&) = delete;
  PerfettoSqlDatabase& operator=(PerfettoSqlDatabase&&) = delete;

  // RAII guard returned by `TryClaimInclude`. Indicates the caller
  // owns an in-flight slot for the module key and must call
  // `Release(success)` (or destruct, treated as failure) so other
  // waiters are unblocked.
  class IncludeClaim {
   public:
    IncludeClaim() = default;
    IncludeClaim(PerfettoSqlDatabase* area, std::string key)
        : area_(area), key_(std::move(key)) {}
    IncludeClaim(IncludeClaim&& o) noexcept
        : area_(std::exchange(o.area_, nullptr)), key_(std::move(o.key_)) {}
    IncludeClaim& operator=(IncludeClaim&& o) noexcept {
      if (this != &o) {
        Reset(/*success=*/false);
        area_ = std::exchange(o.area_, nullptr);
        key_ = std::move(o.key_);
      }
      return *this;
    }
    ~IncludeClaim() { Reset(/*success=*/false); }
    IncludeClaim(const IncludeClaim&) = delete;
    IncludeClaim& operator=(const IncludeClaim&) = delete;

    // Releases the slot. `success=true` also marks the module included
    // so future `TryClaimInclude` calls short-circuit.
    void Release(bool success) { Reset(success); }

   private:
    void Reset(bool success);
    PerfettoSqlDatabase* area_ = nullptr;
    std::string key_;
  };
  struct IncludeClaimResult {
    // Owned-and-active iff `claimed`. The caller runs the include body
    // with this guard alive and calls `Release(success)` at completion.
    IncludeClaim claim;
    // True if the module has already been imported by some connection.
    // Caller must short-circuit before re-running the body — re-running
    // would conflict with the now-promoted shared schema.
    bool already_included = false;
  };

  // Tries to claim the include slot for `module_name`. Blocks until no
  // other thread is mid-import for the same key. Self-recursive
  // includes (same thread already importing this key) deadlock by
  // design — the caller is responsible for short-circuiting via the
  // per-engine `RegisteredPackage::ModuleFile::included` flag *before*
  // calling here.
  IncludeClaimResult TryClaimInclude(const std::string& module_name);

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

  // Function pool — additive-only registry of dynamic PerfettoSQL
  // functions. Each connection tracks `last_synced_function_version_` and
  // diffs against the pool at the top of `Execute`, registering missing
  // entries on its own `sqlite3*`. Replacement (`CREATE OR REPLACE`)
  // appends a fresh entry with `replace=true`; SQLite's normal function
  // re-registration semantics handle the in-engine overwrite.
  //
  // Scope of v1: scalar dynamic functions only. Functions returning a
  // TABLE go through `RuntimeTableFunctionModule` (state carries an
  // engine pointer; not yet cross-connection coherent). Static built-in
  // functions are not in this pool.
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
  struct FunctionPoolSnapshot {
    std::vector<FunctionPoolEntry> entries;
    uint64_t latest_version = 0;
  };
  // Returns the new latest version. Callers must invoke this *after* the
  // function has been successfully registered on the writer's own
  // `sqlite3*` so that an early-failed registration does not leak a
  // stale entry into the pool.
  uint64_t AppendFunction(FunctionPoolEntry entry);
  // Cheap fast-path: returns an empty entries list and `latest_version
  // == since_version` when the caller is already up-to-date.
  FunctionPoolSnapshot SnapshotSince(uint64_t since_version) const;
  uint64_t LatestFunctionVersion() const;
  // Used by `RestoreInitialTables` (which already requires zero
  // secondary connections). Calling this while any reader engine has a
  // non-zero `last_synced_version_` is unsafe.
  void ResetFunctionPool();

  // Package pool — additive-only registry of `RegisterSqlPackage` calls.
  // Mirrors the function pool: each connection tracks
  // `last_synced_package_version_` and diffs at `Execute` start. Stores
  // raw (name, sql) module pairs rather than the move-only
  // `RegisteredPackage` so multiple readers can rebuild their own
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
  struct PackagePoolSnapshot {
    std::vector<PackagePoolEntry> entries;
    uint64_t latest_version = 0;
  };
  uint64_t AppendPackage(PackagePoolEntry entry);
  PackagePoolSnapshot SnapshotPackagesSince(uint64_t since_version) const;
  uint64_t LatestPackageVersion() const;
  void ResetPackagePool();

  // Reset the cross-connection "already included" set. Called from
  // `RestoreInitialTables` (which already requires zero secondary
  // connections, so no in-flight claims).
  void ResetIncludedModules();

 private:
  friend class IncludeClaim;
  void ReleaseClaim(const std::string& key, bool success);

  static std::string MakeVtabKey(const std::string& module_name,
                                 const std::string& vtab_name);

  // Single mutex + condvar coordinate the include-claim machinery and
  // protect the `already-included` set. `in_progress_` records keys
  // currently being imported by some thread; waiters wake on `cv_` when
  // a slot is released.
  mutable std::mutex include_mu_;
  std::condition_variable include_cv_;
  std::unordered_set<std::string> in_progress_;
  std::unordered_set<std::string> included_modules_;

  mutable std::mutex vtab_state_mutex_;
  base::FlatHashMap<std::string, std::shared_ptr<void>> vtab_state_;

  // The latest version equals `<pool>.size()` under the additive-only
  // invariant; the atomic exposes it to the diff fast-path which can
  // skip the lock when nothing new has been appended.
  mutable std::mutex function_pool_mutex_;
  std::vector<FunctionPoolEntry> function_pool_;
  std::atomic<uint64_t> function_pool_version_{0};

  mutable std::mutex package_pool_mutex_;
  std::vector<PackagePoolEntry> package_pool_;
  std::atomic<uint64_t> package_pool_version_{0};
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_GLOBAL_STAGING_AREA_H_
