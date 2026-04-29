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
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/perfetto_sql/parser/function_util.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/util/sql_argument.h"

namespace perfetto::trace_processor {

// Cross-connection state shared by every PerfettoSqlEngine attached to the
// same TraceProcessorImpl. Owned by TraceProcessorImpl and passed by pointer
// into each connection at construction time.
//
// In the multi-connection design this object holds:
//   - the vtab-state map, populated on writer `OnCommit` and consulted by
//     reader connections during cold xConnect;
//   - the function pool, an additive-only registry diffed against by each
//     connection at the start of `Execute` (no DROP, ever);
//   - per-module include locks that serialise concurrent
//     `INCLUDE PERFETTO MODULE` invocations against the same module name.
//
// Phase 2 iter 3 fills in the per-module include lock map. The vtab-state
// map and function pool remain TODOs for later chunks.
class GlobalStagingArea {
 public:
  // RAII guard returned by `AcquireIncludeLock`. Holds a `std::unique_lock`
  // on the per-module mutex; the lock is released on destruction.
  //
  // Concurrency note: Phase 2 is single-threaded so contention is not yet
  // possible. The API exists now so Phase 3 (thread safety) can wire
  // multi-threaded RPC fan-out without re-plumbing include processing.
  class IncludeLockGuard {
   public:
    IncludeLockGuard() = default;
    explicit IncludeLockGuard(std::unique_lock<std::mutex> lock)
        : lock_(std::move(lock)) {}

    IncludeLockGuard(IncludeLockGuard&&) = default;
    IncludeLockGuard& operator=(IncludeLockGuard&&) = default;

    IncludeLockGuard(const IncludeLockGuard&) = delete;
    IncludeLockGuard& operator=(const IncludeLockGuard&) = delete;

   private:
    std::unique_lock<std::mutex> lock_;
  };

  GlobalStagingArea();
  ~GlobalStagingArea();

  GlobalStagingArea(const GlobalStagingArea&) = delete;
  GlobalStagingArea& operator=(const GlobalStagingArea&) = delete;

  GlobalStagingArea(GlobalStagingArea&&) = delete;
  GlobalStagingArea& operator=(GlobalStagingArea&&) = delete;

  // Acquire the per-module mutex for `module_name`. Two connections importing
  // the same module serialise here; different modules don't contend. The
  // mutex itself is created on first request (and lives for the lifetime of
  // this `GlobalStagingArea`); the returned `IncludeLockGuard` releases on
  // destruction.
  IncludeLockGuard AcquireIncludeLock(const std::string& module_name);

  // Cross-connection vtab-state map. Keyed by `(module_name, vtab_name)`.
  //
  // The "writer" connection (today: only the default connection) publishes
  // its committed `PerVtabState::committed_state` via `PublishVtabState` from
  // its `OnCommit` hook. Other connections, on cold xConnect for a vtab they
  // haven't seen before, look up the same shared state via `LookupVtabState`
  // and materialise a local `PerVtabState` from it.
  //
  // The stored value is an opaque `shared_ptr<void>` so different vtab
  // modules can store different state types (e.g. `DataframeModule::State`).
  // Producers and consumers must agree on the type; `static_pointer_cast` at
  // the call site keeps the API minimal.
  //
  // Phase 2 is single-threaded so an internal mutex is enough; a cross-thread
  // safe variant is a Phase 3 concern. The shared state itself must remain
  // immutable / append-only after publish (per the dataframe-vtab design
  // rule: dataframes share `shared_ptr` columns/indexes; CREATE INDEX
  // produces a new dataframe rather than mutating in place).
  void PublishVtabState(const std::string& module_name,
                        const std::string& vtab_name,
                        std::shared_ptr<void> state);

  // Removes a previously-published vtab state. No-op if the entry does not
  // exist. Called from the writer's `OnCommit` when a vtab has been
  // dropped.
  void RemoveVtabState(const std::string& module_name,
                       const std::string& vtab_name);

  // Looks up a previously-published vtab state. Returns null if no entry
  // for the given key exists.
  std::shared_ptr<void> LookupVtabState(const std::string& module_name,
                                        const std::string& vtab_name) const;

  // ===========================================================================
  // Function pool (additive only — no DROP).
  // ===========================================================================
  //
  // Records dynamic PerfettoSQL function definitions created via
  // `CREATE PERFETTO FUNCTION`. Each connection's `PerfettoSqlEngine` tracks a
  // `last_synced_version_` and at every top-level `Execute` start diffs
  // against this pool, registering missing entries on its own `sqlite3*`.
  // The pool is append-only: function "replacement" via
  // `CREATE OR REPLACE PERFETTO FUNCTION` shows up as a fresh entry whose
  // `replace` flag is true; the consuming engine handles the in-engine
  // overwrite via SQLite's normal function-registration semantics.
  //
  // Only the writer (default) connection appends; all other connections only
  // consume. Phase 2 is single-threaded so contention against the internal
  // mutex is impossible today; the lock is documented as the Phase 3 hook.
  //
  // Scope of v1: scalar dynamic functions only (i.e. those backed by the
  // `CreatedFunction` user-data path; `RETURNS INT/STRING/...`). Functions
  // returning a TABLE go through `RuntimeTableFunctionModule` whose state
  // carries an engine pointer and is not yet cross-connection coherent —
  // see the iter 4 STATUS deferrals. Static built-in functions (e.g. those
  // registered during engine construction in `InitPerfettoSqlEngine`) are
  // *not* in this pool: replicating them is a separate follow-on chunk.
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

  // Snapshot returned by `SnapshotSince`. `entries` are the pool entries
  // strictly newer than the caller-provided `since_version` (i.e. with index
  // >= since_version under the additive-only invariant). `latest_version`
  // is the new version the caller should record after registering all
  // entries on its handle.
  struct FunctionPoolSnapshot {
    std::vector<FunctionPoolEntry> entries;
    uint64_t latest_version = 0;
  };

  // Append a function definition to the pool. Returns the new latest version
  // (== old latest version + 1). Callers must invoke this *after* the
  // function has been successfully registered on the writer's own
  // `sqlite3*` handle so that an early-failed registration does not leak a
  // stale entry into the pool.
  uint64_t AppendFunction(FunctionPoolEntry entry);

  // Returns all entries appended after `since_version` together with the
  // latest version after the snapshot. Cheap fast-path: returns an empty
  // entries list and `latest_version == since_version` when the caller is
  // already up-to-date.
  FunctionPoolSnapshot SnapshotSince(uint64_t since_version) const;

  // Cheap peek of the latest version. Used by readers to short-circuit the
  // diff scan when no new functions have been appended since the last sync.
  uint64_t LatestFunctionVersion() const;

  // Drops every entry from the function pool and resets the version counter.
  // Used by `RestoreInitialTables` (which already requires that no secondary
  // connections are alive) to wipe stale entries before the writer's fresh
  // engine re-runs the prelude. Calling this while any reader engine has
  // a non-zero `last_synced_version_` is unsafe — the reader would think
  // it had registered all entries when in fact the new pool is empty.
  void ResetFunctionPool();

 private:
  static std::string MakeVtabKey(const std::string& module_name,
                                 const std::string& vtab_name);

  std::mutex map_mutex_;
  std::unordered_map<std::string, std::unique_ptr<std::mutex>> module_locks_;

  mutable std::mutex vtab_state_mutex_;
  base::FlatHashMap<std::string, std::shared_ptr<void>> vtab_state_;

  // Function pool. The latest version equals `function_pool_.size()` under
  // the additive-only invariant; we expose it via an atomic for cheap
  // lock-free peeks from the diff fast-path.
  mutable std::mutex function_pool_mutex_;
  std::vector<FunctionPoolEntry> function_pool_;
  std::atomic<uint64_t> function_pool_version_{0};
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_GLOBAL_STAGING_AREA_H_
