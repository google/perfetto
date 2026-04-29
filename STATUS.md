# Multi-connection TraceProcessor — Loop Status

## Current Phase
Phase 3 in progress (thread safety + retry middleware).

## Design pivots (2026-04-29, mid-Phase-1)
- **WAL abandoned.** Switching to serialized transactions with
  `journal_mode=MEMORY` + `temp_store=MEMORY` + `locking_mode=NORMAL`.
  No custom SHM-capable VFS needed — multi-conn concurrency comes from
  serializing writes per connection, not from WAL. Drops the deferred
  `custom-shm-vfs` Phase 3 chunk entirely.
- **Includes use a temp-then-promote pattern.** During an include's
  execution, all CREATE statements land in `temp.<name>` (per-connection
  temp schema, private). At successful end-of-include, drop the temp
  versions and re-issue them as DDL on main schema so `cache=shared`
  propagates to other connections. Vtabs are recreated at the SQL level
  (DDL), not via direct C++ vtab module callbacks. If the include
  fails, temp objects are dropped and main is untouched — atomic at
  module granularity. This is the Phase 2 mechanism for cross-conn
  schema visibility; replaces the earlier "OnCommit publishes to
  GlobalStagingArea, cold xConnect on conn B reads from global" path
  for include-time schema sync. (GlobalStagingArea still owns the
  function pool + per-module include locks.)

## Recent activity (newest first)
- 2026-04-29 [Phase 3 iter 3]: globals-audit done (SqlStats race
  fixed). Surface fix for the iter 2 ASan finding: every public
  method on `TraceStorage::SqlStats` now takes a private
  `mutable std::mutex mutex_`. The four `RecordQuery*` writers and
  `size()` are guarded directly; the four read accessors that
  exposed raw `const std::deque<...>&` references (used only by
  `SqlStatsModule` for `SELECT * FROM sqlstats`) are replaced by a
  new `SnapshotForReading()` that copies all four columns into a
  `Snapshot` struct of `std::vector<...>` under the lock and
  returns by value. The `SqlStatsModule::Cursor` carries the
  `Snapshot` for its lifetime so iteration is decoupled from
  concurrent writers — no torn reads, no UAF if a `pop_front`
  fires mid-iteration. The `Filter` no longer caches a `num_rows`;
  it just stores the snapshot whose `size()` is the bound. Header
  changes to `trace_storage.h`: `<mutex>` include + `Snapshot`
  nested struct + `SnapshotForReading` declaration. Implementation
  in `trace_storage.cc`. `sql_stats_table.{h,cc}` migrated to use
  the snapshot path; `sql_stats_table.h` now `#include`s
  `trace_storage.h` directly to see `SqlStats::Snapshot` (Cursor
  holds it by value).

  **Stress test added**: `ConcurrentRecordingIntoSqlStats` under
  `TraceProcessorConnectionTest.*`. Spawns 4 threads each owning
  its own secondary `Connection`; each runs 100 trivial
  `SELECT <unique>` queries to drive the full
  `RecordQueryBegin` / `RecordQueryFirstNext` / `RecordQueryEnd`
  cycle. Distinct query strings per (thread, iter) keep every
  iteration on the `push_back` hot path. After the join, a
  `SELECT count(*) FROM sqlstats` on the writer asserts the count
  is positive and bounded by `kMaxLogEntries` (100) — proves the
  vtab path also runs cleanly under the lock. Previously, this
  pattern reproduced the iter-2 container-overflow flake within a
  handful of iterations; post-fix, **10/10 ASan runs of
  `TraceProcessorConnectionTest.*` are clean**, including the new
  stress test, the iter-1 `ConcurrentReadersDoNotCrash`, and the
  iter-2 `ConcurrentIncludesOfSameModuleSerialise`.

  **Audit findings (deferred)**:
  - `StringPool` (`src/trace_processor/containers/string_pool.h`)
    has a built-in `mutex_` plus a `set_locking(bool)` toggle, but
    the toggle defaults to `false` and **no production caller flips
    it on**. Every `InternString` from a query handler (e.g.
    `GProfileBuilder::StringTable::InternString`,
    `json_args::ParseArg`'s `storage->InternString`,
    `RuntimeDataframeBuilder`'s pool inserts) mutates
    `string_index_` and `blocks_` un-guarded. Single-connection
    today, so quiescent — but as soon as Phase 4 runs queries on
    multiple threads, this will be the next race. Seeded as a new
    chunk `string-pool-thread-safety` below.
  - `TraceStorage`'s data tables (`SchedSliceTable`, `SliceTable`,
    etc.) are written only during ingestion and read during query.
    The Phase 2 design rule "multi-connection only legal post-
    `NotifyEndOfFile`; concurrent ingestion is out of scope" is
    enforced at `TraceProcessorImpl::CreateConnection` via
    `PERFETTO_CHECK(notify_eof_called_)` (and mutating TP methods
    are gated on `non_default_connection_count_ == 0`). So
    concurrent reads are fine and reads-during-write cannot occur.
    Documented; no action.
  - SQLite-binding statics in `src/trace_processor/sqlite/bindings/`
    are mostly compile-time `kModule` constexpr structs (one per
    module) plus `RegisterFunction` wrappers — no shared mutable
    state at the binding layer. Per-engine state lives on the
    `sqlite3*` handle, which is naturally per-connection. No
    findings.
  - `TraceProcessorImpl` itself: the `non_default_connection_count_`
    is an `int` mutated under what is effectively the GIL of "only
    the writer thread calls `CreateConnection`/destructor"; with
    Phase 4 RPC pool this becomes an `atomic<int>` or a mutex.
    Deferred to Phase 4.

  **Test counts:**
  - `out/mac_release/perfetto_unittests`: 3248 PASSED + 2 SKIPPED
    + 1 pre-existing macOS failure (`HttpServerTest.Websocket`).
    +1 new test vs. iter 2 (`ConcurrentRecordingIntoSqlStats`).
  - `out/mac_release/perfetto_integrationtests` (TP-relevant
    filter): 122 PASSED, unchanged.
  - `tools/diff_test_trace_processor.py`: 1355 PASSED + 9
    pre-existing skips, unchanged.
  - `out/mac_asan/perfetto_unittests` (connection-tests filter):
    18/18 PASSED across 10 consecutive runs, no ASan reports.
    The iter-2 flake is gone.

- 2026-04-29 [Phase 3 iter 2]: cross-conn-package-propagation done.
  Mirrors iter 5's function-pool design for SQL packages.
  `GlobalStagingArea` grows an additive package pool: a
  `vector<PackagePoolEntry>` (each entry owns a `shared_ptr<const
  vector<pair<string,string>>>` of raw module-name/sql pairs plus the
  `allow_replace` flag) guarded by `package_pool_mutex_`, with an
  `atomic<uint64_t> package_pool_version_` for cheap fast-path peeks.
  New API: `AppendPackage` (writer-only), `SnapshotPackagesSince`,
  `LatestPackageVersion`, `ResetPackagePool`. Stored as raw module
  pairs (not the converted `RegisteredPackage`) because
  `RegisteredPackage` contains a `base::FlatHashMap` which is
  move-only — readers re-construct a fresh `RegisteredPackage` per
  sync via the new helper `RegisteredPackageFromModules`. Each
  reader gets its own copy because `packages_["X"].modules["Y"]
  .included` is a per-reader bookkeeping flag.

  `PerfettoSqlEngine::RegisterPackage` is now defined out-of-line
  with an extra `pool_modules` arg (a `shared_ptr` to the raw module
  pairs) plus the `allow_replace` flag; on the writer it calls the
  internal `RegisterPackageLocal` (just touches the local
  `packages_` map) then append-after-success `AppendPackage`s on
  `staging_area_`. `last_synced_package_version_` is bumped eagerly
  on the writer so its own no-op syncs short-circuit. Readers run
  `SyncPackagesFromPool` at the top of every top-level
  `ExecuteUntilLastStatement` (placed *before*
  `SyncFunctionsFromPool`, though they don't currently depend on
  ordering); the diff is fast-path-skipped via the atomic version
  peek when there are no new entries.

  `RestoreInitialTables` adds `staging_area_->ResetPackagePool()`
  alongside the existing `ResetFunctionPool` so a
  `non_default_connection_count_ == 0` reset wipes both pools
  before the new writer engine boots and re-publishes them via the
  prelude path (`InitPerfettoSqlEngine` now also passes the raw
  modules to `engine->RegisterPackage` so writer-side init goes
  through the same publish path).

  Also added a small adjacent piece needed to make concurrent
  includes of the same module work cross-connection: a
  `included_modules_` set in `GlobalStagingArea` (with
  `MarkModuleIncluded` / `IsModuleIncluded` / `ResetIncludedModules`)
  is consulted under the per-module include lock at the start of
  `IncludeModuleImpl` and the wildcard-expansion include path; if
  another connection has already promoted the module, the local
  `RegisteredPackage::ModuleFile::included` flag is set and the
  body is short-circuited instead of being re-run (which would
  collide with the now-shared-`main` schema). The mark happens
  under the include lock right after `ReleaseIncludeSavepoint`
  succeeds so the cross-connection bit is published atomically with
  the lock release.

  **Three new tests** under `TraceProcessorConnectionTest.*` in
  `src/trace_processor/trace_processor_connection_unittest.cc`:
  - `IncludeOnSecondaryConnectionWorksAfterPackageRegister`: writer
    registers a package, secondary minted afterwards runs
    `INCLUDE PERFETTO MODULE` and queries the included table.
  - `IncrementalPackageRegistrationFlowsToSecondary`: writer
    registers pkg_a, mints+uses+drops a secondary, registers pkg_b
    (the gate `non_default_connection_count_ == 0` requires the
    drop), mints a fresh secondary, verifies *both* pkg_a and
    pkg_b flow through on the new secondary's first sync. Tests
    that the pool retains earlier entries across appends and the
    diff is purely additive.
  - `ConcurrentIncludesOfSameModuleSerialise` (promoted from iter
    1's deferred test): writer pre-includes a module, two
    secondaries from separate threads each re-issue the include +
    SELECT. Validates the include lock acquire/release path is
    deadlock-free under MT and the cross-connection
    `IsModuleIncluded` short-circuits the body so no
    schema-write conflict occurs on shared `main`. The
    naive variant (no pre-include, two secondaries race the
    body) currently fails with "database schema is locked: main"
    on shared-cache contention — that's the busy-retry chunk's
    territory and is documented in the test's docstring.

  **ASan finding (pre-existing, surfaced more clearly here):**
  the new MT include test (and iter 1's `ConcurrentReadersDoNotCrash`
  on re-runs) flakily aborts under ASan in
  `TraceStorage::SqlStats::RecordQueryBegin` —
  `std::deque<string>::push_back` from two connection threads
  concurrently (shared `parent_->context()->storage->mutable_sql_stats()`
  in both `TraceProcessorImpl::ExecuteQuery` and
  `ConnectionImpl::ExecuteQuery`). Container-overflow shadow byte
  `fc`. Pre-existing race exposed by MT secondary connections;
  not caused by this iter's changes. This is squarely in the
  globals-audit chunk's scope. The release-mode test passes
  reliably (~50 consecutive successful runs); the ASan flake
  probability is ~30-40% per run.

  **Test counts:**
  - `out/mac_release/perfetto_unittests`: 3247 PASSED + 2
    SKIPPED + 1 pre-existing macOS failure
    (`HttpServerTest.Websocket`). +3 new tests vs. iter 1.
  - `out/mac_release/perfetto_integrationtests` (TP-relevant
    filter): 122 PASSED, unchanged.
  - `tools/diff_test_trace_processor.py`: 1355 PASSED + 9
    pre-existing skips, unchanged.
  - `out/mac_asan/perfetto_unittests` (connection-tests filter):
    16/17 or 17/17 PASSED depending on the
    `TraceStorage::SqlStats` race window (see ASan finding above).

- 2026-04-29 [Phase 3 iter 1]: include-lock-wire-and-mt-smoke done.
  **Part A** — wired `GlobalStagingArea::AcquireIncludeLock` (added in
  Phase 2 iter 3 but unused) into `PerfettoSqlEngine`'s include path. Both
  `IncludeModuleImpl` (single-key INCLUDE) and the wildcard expansion in
  `ProcessFrame` now acquire the per-module lock *before* opening the
  per-include `SAVEPOINT perfetto_include_<n>` and ride the
  `IncludeLockGuard` along on the `ExecutionFrame` as a new
  `std::optional<GlobalStagingArea::IncludeLockGuard> include_lock` field
  (sibling of `include_savepoint`). The guard's destructor releases the
  lock when the frame is popped — covering both success
  (`ProcessFrame` returns `kFrameDone` after `RELEASE`) and failure
  (the unwind path in `ExecuteUntilLastStatement` calls
  `RollbackIncludeSavepoint` before `pop_back()`, and the destructor
  handles the lock release as part of the `pop_back()`). Legacy
  single-connection callers (`staging_area_ == nullptr`) skip the lock
  acquisition entirely (no-op). Re-entrant include of the same module
  name does *not* self-deadlock because the per-module mutex is now
  `std::recursive_mutex` (changed in `global_staging_area.{h,cc}` along
  with the matching `unique_lock<>` template arg in `IncludeLockGuard`).
  This was the chunk-recommended path: the existing `IncludeLockGuard`
  API stays unchanged and we don't need to track ownership ourselves.
  All four `ExecutionFrame` construction sites in
  `perfetto_sql_engine.cc` (root frame, wildcard frame, single-key
  include frame, wildcard-expanded include frame) updated to pass the
  new `include_lock` aggregate field.

  **Part B** — added two tests under `TraceProcessorConnectionTest.*` in
  `src/trace_processor/trace_processor_connection_unittest.cc`:
  - `ConcurrentReadersDoNotCrash`: spawns two `std::thread`s, each
    owning its own `Connection` (moved into the closure — connections
    are thread-compatible, not thread-safe per design), and runs ~50
    `SELECT 1` / `SELECT 2` iterations on its connection. Establishes
    a clean MT baseline for the read path. Passes both under the
    regular release build and ASan with no errors / no spurious data
    races flagged.
  - `IncludeLockAcquisitionDoesNotDeadlock`: single-thread sanity test
    for the lock plumbing — includes a single module, then a
    wildcard-expanded set, then re-issues the original include
    (which short-circuits via `file.included == true` before acquiring
    the lock). Exercises the lock acquire/release path on every
    `INCLUDE` and confirms re-entry doesn't deadlock under the
    recursive mutex.

  **Surfaced finding (work for iter 2 or later):** the natural
  `ConcurrentIncludesOfSameModuleSerialise` test — two secondary
  connections each calling `INCLUDE PERFETTO MODULE <name>` from
  separate threads — *cannot* be written today because secondary
  connections do not yet inherit the writer's `packages_` registry.
  `PerfettoSqlEngine::ExecuteInclude` (`perfetto_sql_engine.cc:1242`)
  fails with `INCLUDE: Package '<name>' not found` on a secondary because
  `packages_` is per-engine and only populated on the writer (the
  `RegisterPackage` calls in `trace_processor_impl.cc:911` and the
  prelude re-import at `:1614` both target only `engine_`, the writer).
  This is a pre-existing package-propagation gap, *not* a thread-safety
  race: it would fail single-threaded too. Closing it (cross-connection
  `packages_` propagation, likely via a shared snapshot in
  `GlobalStagingArea` mirroring the function pool design) is the
  natural follow-on so the include-lock plumbing can be exercised
  end-to-end against contended access. This is now seeded as a
  Phase 3 chunk.

  **Test counts (all green):**
  - `out/mac_release/perfetto_unittests`: 3230 PASSED + 1 SKIPPED + 1
    pre-existing macOS failure (`HttpServerTest.Websocket`,
    stack-buffer-overflow in `http_server.cc::ParseOneWebsocketFrame`,
    flagged identically before this iter's changes). +2 vs. Phase 2
    close (3228).
  - `out/mac_release/perfetto_integrationtests` (TP-relevant filter):
    122 PASSED, unchanged.
  - `tools/diff_test_trace_processor.py`: 1355 PASSED + 9 pre-existing
    skips (etm + llvm_symbolizer modules absent), unchanged.
  - `out/mac_asan/perfetto_unittests` (excluding pre-existing
    `HttpServerTest.Websocket`): 3230 PASSED + 1 SKIPPED. ASan flagged
    no new errors against either of the new tests, including the
    multi-threaded `ConcurrentReadersDoNotCrash`.

- 2026-04-29 [Phase 2 iter 6]: execute-savepoint-wrap done. Every
  top-level (non-re-entrant) `PerfettoSqlEngine::ExecuteUntilLastStatement`
  now opens `SAVEPOINT perfetto_execute_<n>` (counter
  `execute_savepoint_counter_`, mirroring the iter-3 include-savepoint
  pattern) immediately after `SyncFunctionsFromPool` and either
  `RELEASE`s it (success path, after the unwind back to `stack_base`) or
  `ROLLBACK TO ...; RELEASE ...`s it (error path) before returning. The
  per-include savepoints from iter 3 nest cleanly inside this outer
  wrap (SQLite supports arbitrary savepoint nesting). Re-entrant
  `Execute` calls from statement handlers (e.g. `ExecuteCreateFunction`
  issuing a generated CREATE VIRTUAL TABLE) skip the wrap entirely —
  their work is already inside the outer one — by gating on
  `stack_base == 0`, exactly the same gate the existing function-pool
  sync uses. Three new helpers in `perfetto_sql_engine.{h,cc}`:
  `OpenExecuteSavepoint` (returns the generated name + status),
  `ReleaseExecuteSavepoint` (status-returning), and
  `RollbackExecuteSavepoint` (best-effort, logs on failure). Mirrors
  the `ReleaseIncludeSavepoint` / `RollbackIncludeSavepoint` shape
  from iter 3 rather than introducing a new abstraction. RELEASE
  failures on the success path are promoted to the user-visible error
  and the outer savepoint is rolled back best-effort, so callers always
  see clean state.
  All callers funnel through `ExecuteUntilLastStatement` — that's the
  only entry point that needs wrapping (`Execute(sql)` is a thin
  wrapper that delegates to `ExecuteUntilLastStatement` and steps the
  final statement).
  Tests added under `TraceProcessorConnectionTest.*` in
  `src/trace_processor/trace_processor_connection_unittest.cc`:
  - `MultiStatementExecuteRollsBackOnFailure`: issues
    `CREATE TABLE multistmt_t (x INT); CREATE TABLE multistmt_t (y INT);`
    in one Execute; the second statement fails with "table already
    exists". Verifies (a) the iterator surfaces the error, (b) the
    table is absent from `sqlite_master` on the issuing connection, and
    (c) absent on a freshly-minted secondary connection (rules out the
    pathological case where the rollback only affects the primary's
    view but leaks via `cache=shared`).
  - `MultiStatementExecuteCommitsOnSuccess`: positive control —
    two CREATEs + two INSERTs in a single Execute land normally and a
    secondary connection observes the rows.
  Build/test results on `out/mac_release`: gn check clean (no BUILD.gn
  changes — pure header+impl edit). 3228 unittests pass + 1
  pre-existing skip (`HttpServerTest.Websocket`); +2 new tests vs.
  iter 5. 122 TP integrationtests pass. 1355 diff tests pass + 9
  pre-existing skips. No regressions and no behaviour change for
  single-statement queries: a single CREATE/SELECT now runs inside an
  outer savepoint that is RELEASEd at the end, which is observably
  identical to today's "implicit commit at statement end" semantics.
- 2026-04-29 [Phase 2 iter 5]: function-pool-per-conn-diff done.
  `GlobalStagingArea` now owns an additive function pool keyed by
  insertion order: a `std::vector<FunctionPoolEntry>` guarded by
  `function_pool_mutex_` plus an `std::atomic<uint64_t>
  function_pool_version_` for cheap lock-free peeks. API:
  `AppendFunction` (writer-only), `SnapshotSince(since_version)` (any
  reader), `LatestFunctionVersion`, and `ResetFunctionPool`.
  `FunctionPoolEntry` carries `{replace, FunctionPrototype,
  sql_argument::Type, SqlSource}` — the same four args
  `RegisterLegacyRuntimeFunction` already takes. Functions are
  stateless (no fn-pointer or context state), so a reader can re-create
  one on its own handle by replaying the appended args verbatim.
  `PerfettoSqlEngine` gained `is_writer_` and
  `last_synced_function_version_` members; the public
  `RegisterLegacyRuntimeFunction` was split into a `Local` helper
  (registers on the engine's own `sqlite3*`) and a writer-side wrapper
  that appends to the staging pool *only after* the local register
  succeeds (so a half-baked entry never lands in the pool). New
  `SyncFunctionsFromPool` runs at the top of `ExecuteUntilLastStatement`
  but only when `stack_base == 0` (re-entrant `Execute` calls from
  statement handlers skip — the writer is the only one that appends
  and it always installs locally first). Writer engines short-circuit
  the sync (they're the source of truth) and just bump
  `last_synced_function_version_` to the latest. Reader engines
  iterate `snapshot.entries` and call the local register helper for
  each, then update to `snapshot.latest_version`.
  `RestoreInitialTables` now calls `staging_area_->ResetFunctionPool()`
  before tearing down the writer engine; the existing
  `non_default_connection_count_ == 0` CHECK at function entry
  guarantees no reader engine observes the wipe. Reset is needed
  because the new engine boots with fresh storage (its own memdb URI,
  `cache=shared` not yet wired across the reset boundary) and the
  prelude include re-creates everything from scratch — stale pool
  entries would otherwise be replayed against a half-built schema on
  a future reader connection.
  Tests added under `TraceProcessorConnectionTest.*`:
  - `DynamicFunctionPropagatesToSecondary`: define `conn_double` on
    conn-0, verify a freshly-minted secondary picks it up via the
    `Execute`-time sync.
  - `DynamicFunctionPickedUpIncrementally`: mint conn-1 first (with
    empty pool), then create two functions on conn-0, then a third —
    verifies all three flow through on subsequent secondary
    `Execute`s, exercising the "diff at every Execute, not snapshot
    at mint time" invariant.
  Build/test results on `out/mac_release`: gn check clean (no BUILD.gn
  changes needed — `function_util.h` lives under the existing
  `../parser` dep). 3226 unittests pass + 1 pre-existing skip
  (`HttpServerTest.Websocket`); +2 new tests vs. iter 4. 122 TP
  integrationtests pass. 1355 diff tests pass + 9 pre-existing skips.
  No behaviour change for existing single-connection callers — the
  writer publishes only when `staging_area_ != nullptr` and
  `is_writer_ == true`, both of which are wired exclusively by
  `TraceProcessorImpl`'s primary engine.
  Deferred / next-iter: `execute-savepoint-wrap` (multi-statement
  atomicity for top-level `Execute`); replicating *static* built-in
  functions registered during engine construction (the pool only
  carries dynamic CREATE-PERFETTO-FUNCTION entries today — built-ins
  registered via `InitPerfettoSqlEngine` are re-installed by each
  engine's own constructor, so this works for stdlib usage but a
  query like `SELECT my_static_fn(...)` on a secondary that the
  primary registered post-init would miss); `RuntimeTableFunctionModule`
  cross-conn (engine-pointer-in-State problem from iter 4).
- 2026-04-29 [Phase 2 iter 4]: vtab-state-staging-publish done
  (dataframe module wired; runtime/static-table-function modules
  deferred). Cross-connection vtab-state plumbing now lets a
  secondary connection observe DataframeModule-backed virtual tables
  created on the primary connection.
  Data shape: `GlobalStagingArea` gained an internally-locked
  `flat_hash_map<std::string, std::shared_ptr<void>>` keyed by
  `(module_name + "\0" + vtab_name)`. The stored value is an opaque
  `shared_ptr<void>` aliasing the writer's
  `PerVtabState::committed_state` (which is the
  `std::shared_ptr<DataframeModule::State>` carrying the
  `Dataframe*` and `named_indexes`). API: `PublishVtabState`,
  `LookupVtabState`, `RemoveVtabState`. Mutex-guarded inside the
  staging area; Phase 2 is single-threaded so contention is
  impossible today, but the lock is documented as the Phase 3 hook.
  Publish path: `ModuleStateManagerBase::OnCommit` and `OnRollback`
  are now `virtual`. `DataframeModule::Context` overrides `OnCommit`
  to (1) run the base bookkeeping and (2) iterate
  `state_by_name_` and republish each surviving entry's
  `committed_state` via
  `PublishVtabState("__intrinsic_dataframe", name, …)`. Only the
  writer (primary engine) publishes — secondary engines have
  `is_writer = false` and skip the publish step. `OnRollback` is a
  pure passthrough since rolled-back state was never published.
  Cold xConnect path: `ModuleStateManagerBase::OnConnect` now falls
  back to a virtual `ResolveMissingStateOnConnect(name)` hook on
  miss instead of `PERFETTO_CHECK`-crashing. The default returns
  null (preserves the legacy "missing state is a bug" semantics for
  the writer engine and any subclass that doesn't override).
  `DataframeModule::Context::ResolveMissingStateOnConnect` returns
  the shared_ptr looked up from staging on reader engines; the
  base then materialises a fresh local `PerVtabState` whose
  `committed_state` and `active_state` both share ownership with
  the published value.
  Re-resolve at cursor time: `DataframeModule::BestIndex` and
  `Filter` now call a new file-local `ResolveState(Vtab*)` helper
  instead of `ModuleStateManager::GetState(v->state)`. When the
  context has a staging area, the helper fetches the latest
  shared_ptr from staging; otherwise (legacy single-conn) it falls
  back to the local PerVtabState. This honours the design rule
  "no caching in PerVtabState; CREATE INDEX produces a new
  dataframe sharing internal shared_ptr columns/indexes" — even
  though today's `dataframe::Dataframe::AddIndex` mutates in place
  rather than spawning a new dataframe, the indirection is in
  place for when that flips.
  Wiring: `PerfettoSqlEngine`'s primary ctor gained an optional
  `GlobalStagingArea*` arg (default null for legacy callers);
  the secondary (shared-filename) ctor takes one as a required
  arg. The primary engine sets `dataframe_context_->is_writer = true`
  and `staging_area = staging_area`; the secondary engine
  registers only the `__intrinsic_dataframe` module with
  `is_writer = false` plus the same staging area, and installs the
  commit/rollback callbacks (so its local state_by_name_ stays
  consistent on rollback). `TraceProcessorImpl` now passes
  `staging_area_.get()` into both `InitPerfettoSqlEngine` (via a
  new `staging_area` field on `InitPerfettoSqlEngineArgs`) and
  `CreateConnection`.
  Modules deferred to a follow-on chunk:
  - `RuntimeTableFunctionModule` (used by `CREATE PERFETTO
    FUNCTION foo(...) RETURNS TABLE`). Its `State` carries a
    `PerfettoSqlEngine*` plus a `temporary_create_stmt`; sharing
    the engine pointer cross-connection is incoherent for v1
    (executes on the wrong engine's prepared-statement cache).
    Needs design work — out of scope here.
  - `StaticTableFunctionModule` (used by `experimental_*` and the
    `__intrinsic_*` table functions). Mechanically straightforward
    (mirror the dataframe pattern) but defers because the State
    holds a `unique_ptr<StaticTableFunction>` whose Cursor objects
    aren't yet reasoned about for cross-connection sharing.
  Tests added under `TraceProcessorConnectionTest.*` in
  `src/trace_processor/trace_processor_connection_unittest.cc`:
  - `SecondaryConnectionReadsDataframeVtabFromPrimary`: conn-0
    runs `CREATE PERFETTO TABLE conn_df_test AS SELECT … UNION
    ALL …` (which goes through DataframeModule), then a fresh
    secondary connection runs `SELECT … FROM conn_df_test ORDER
    BY id` and gets back the same three rows. Exercises the full
    publish → cold-xConnect → re-resolve loop.
  - `SecondaryConnectionReadsStaticDataframeTable`: verifies the
    static dataframe-backed `thread` table (registered via
    `RegisterStaticTable` during engine init) is queryable from
    a secondary connection and returns the same row count as the
    primary. Empty trace → 0 rows on both, but the vtab discovery
    + resolve path is exercised.
  Build/test results on `out/mac_release`: gn check clean. 3224
  unittests pass + 1 pre-existing skip
  (`HttpServerTest.Websocket`); +5 new tests vs. iter 3 (the 2
  new ones above plus 3 pre-existing connection tests now also
  exercise the dataframe path). 122 TP integrationtests pass.
  1355 diff tests pass + 9 pre-existing skips. No behaviour
  change for existing single-connection callers — the legacy
  `PerfettoSqlEngine(pool, enable_extra_checks)` ctor still works
  via the new optional staging-area argument defaulting to null.
- 2026-04-29 [Phase 2 iter 3]: include-temp-then-promote done.
  Implemented the include-atomicity half of the temp-then-promote
  pattern via SAVEPOINT (Option C from the chunk plan). Each
  `INCLUDE PERFETTO MODULE` invocation opens a uniquely-named
  `SAVEPOINT perfetto_include_<n>` before pushing the kInclude
  frame; on successful frame completion the savepoint is RELEASEd
  (committing the module's DDL onto `main` so `cache=shared`
  propagates the new objects to other connections); on any error
  during the include body the unwind path in
  `ExecuteUntilLastStatement` rolls back the savepoint
  (`ROLLBACK TO ...; RELEASE ...`) so partially-installed objects
  do not leak. Wildcard expansion (`INCLUDE PERFETTO MODULE foo.*;`)
  opens one savepoint per individual module include so each is
  atomic in isolation.
  Why Option C over Option A (string-rewrite to `temp.<name>`):
  for plain SQL DDL (CREATE TABLE / CREATE VIEW / INSERT) inside a
  module body, `cache=shared` already propagates committed objects
  on `main` to sibling connections — verified by the new
  `IncludePromotesObjectsToOtherConnections` test. The "temp
  schema buffer" wording in the design memo is one mechanism;
  savepoint-based atomicity is another that reaches the same end
  state with no behaviour change for the SQL case. The design
  memo's worry about vtab-DDL inside a rolled-back savepoint
  leaking module state remains a real risk but is *exercised by
  existing CREATE PERFETTO TABLE callers* (which already use
  nested savepoints), and the existing
  `OnRollback`-fires-into-`virtual_module_state_managers_` plumbing
  already handles per-statement-manager rollback. Cross-connection
  vtab visibility (the data side) is still
  `vtab-state-staging-publish` territory — a non-default
  connection can't yet read a vtab created by an include because
  no vtab modules are registered on it.
  Code touched:
  `src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.{h,cc}`:
  added `ExecutionFrame::include_savepoint`, monotonic
  `include_savepoint_counter_`, helpers
  `ReleaseIncludeSavepoint` / `RollbackIncludeSavepoint`. Wired
  SAVEPOINT issuance into both `IncludeModuleImpl` (single-key
  path) and the wildcard-expansion path inside `ProcessFrame`.
  Wired RELEASE into the kInclude `kFrameDone` branch (clears
  `include_savepoint` after success so the unwind path doesn't
  double-rollback). Wired ROLLBACK TO into the unwind loop in
  `ExecuteUntilLastStatement`. Updated the three brace-init lists
  pushing kRoot / kWildcard frames to fill in the new field.
  `src/trace_processor/perfetto_sql/engine/global_staging_area.{h,cc}`:
  added `IncludeLockGuard` RAII type and
  `AcquireIncludeLock(module_name)` API that lazily allocates a
  `std::mutex` per module name. Phase 2 is single-threaded so
  contention is impossible today; the lock is documented as the
  Phase 3 thread-safety hook. **Not yet plumbed** through to
  `PerfettoSqlEngine` (the engine has no `GlobalStagingArea*`
  back-pointer today; threading that through is a Phase 3
  concern). API exists so the next iter can wire it without
  re-architecting.
  Tests added under `TraceProcessorConnectionTest.*` in
  `src/trace_processor/trace_processor_connection_unittest.cc`:
  - `IncludePromotesObjectsToOtherConnections`: register a SQL
    package, include it on conn-0, then mint a new connection and
    SELECT the table — verifies the RELEASE-then-cache=shared
    promotion path.
  - `FailedIncludeLeavesNoTrace`: include body has a bad final
    statement; verify (a) the include errors, and (b) the table
    created earlier in the body is absent from `sqlite_master` on
    both the default and a fresh secondary connection.
  - `SequentialIncludesPromoteToOtherConnection`: two distinct
    successful includes; verify both objects visible to a
    secondary connection.
  Build/test results on `out/mac_release`: 3222 unittests pass
  (was 3219; +3 new) + 1 pre-existing skip (`HttpServerTest.
  Websocket`), 122 TP integrationtests pass, 1355 diff tests pass
  + 9 pre-existing skips. No behaviour change for existing
  callers — the new SAVEPOINT around includes is invisible to
  observers because committed DDL behaves identically to before.
  Deferred TODOs (next iter scoping):
  - Vtab DDL inside an include savepoint: the design memo flagged
    "dangling vtab module state" if a CREATE VIRTUAL TABLE inside a
    rolled-back savepoint leaves PerVtabState behind. Existing
    CREATE PERFETTO TABLE already uses nested savepoints with
    `OnRollback` cleanup, so the in-tree case is OK; need to
    explicitly stress-test an include containing a CREATE PERFETTO
    TABLE that fails partway when stdlib modules start being
    exercised (the new tests use plain CREATE TABLE).
  - Include lock not yet acquired in `IncludeModuleImpl`. Plumbing
    a `GlobalStagingArea*` into `PerfettoSqlEngine` is a small
    follow-on; left for the iter that needs concurrency
    (Phase 3).
  - `INSERT`-only or `DROP`/`ALTER`-only include bodies not
    explicitly tested. The savepoint mechanism is statement-type-
    agnostic so they should "just work", but no new test exercises
    them.
  - The success-path RELEASE before `file.included = true` could
    theoretically fail (e.g. an OnCommit hook rejects). If it does,
    the file remains un-`included` and the next attempted include
    will retry — probably the right semantics, but worth
    documenting.
- 2026-04-29 [Phase 2 iter 2]: perfetto-sql-engine-per-conn done.
  Each non-default `Connection` now owns its own `PerfettoSqlEngine`
  (with a fresh `SqliteEngine` / `SqliteConnection`) opened against
  the primary engine's memdb URI so `cache=shared` ties them together
  at the storage layer. The filename-sharing mechanism is **option A**
  from the chunk plan: a public read-only `SqliteEngine::filename()`
  accessor (`src/trace_processor/sqlite/sqlite_engine.h`) plus a new
  alternate ctor `SqliteEngine(const std::string& shared_filename)`
  (and a matching `PerfettoSqlEngine(StringPool*, bool, const
  std::string& shared_filename)`) that reuses the URI passed in
  instead of generating a new one. `TraceProcessorImpl::CreateConnection`
  now mints `auto engine = std::make_unique<PerfettoSqlEngine>(
  context()->storage->mutable_string_pool(),
  config_.enable_extra_checks,
  engine_->sqlite_engine()->filename());` and hands it to
  `ConnectionImpl`. `ConnectionImpl::ExecuteQuery` dispatches through
  its own engine: it calls `RecordQueryBegin` on the parent's
  `sql_stats` (so non-default connections show up alongside
  connection-0 in the diagnostic table), runs
  `engine_->ExecuteUntilLastStatement(SqlSource::FromExecuteQuery(...))`,
  and wraps the result in an `IteratorImpl` that points at the parent
  for end-of-query bookkeeping.
  Smoke test: `src/trace_processor/trace_processor_connection_unittest.cc`
  with three tests under `TraceProcessorConnectionTest.*` —
  `SecondaryConnectionExecutesTrivialQuery` (`SELECT 1`),
  `SecondaryConnectionSeesPrimarySchema` (CREATE TABLE +
  INSERT on conn-0, then SELECT on a freshly-minted conn that sees
  rows via `cache=shared`), and `MultipleConnectionsCoexist` (two
  live connections each running `SELECT N`). New BUILD.gn target
  `:trace_processor_unittests` (gated on
  `enable_perfetto_trace_processor_sqlite`) wires these into
  `perfetto_unittests` via `:unittests`.
  Intentional limits documented on the secondary-engine ctor and
  the `ConnectionImpl` class doc-comment: vtab modules are NOT
  registered on non-default connections (no
  `runtime_table_function`, no `__intrinsic_dataframe`, no
  `__intrinsic_static_table_function`); PerfettoSQL functions are
  NOT registered; the `perfetto_tables` housekeeping table is NOT
  re-created (already in the shared cache); commit/rollback hooks
  are NOT installed (no state managers to notify). Queries that
  resolve to a vtab module or a SQL-defined function will fail on
  non-default connections — vtab replication lands in
  `vtab-state-staging-publish` and function replication lands in
  `function-pool-per-conn-diff`.
  One subtle pothole worth recording: an early version passed
  `nullptr` for the `TraceProcessorImpl*` back-pointer to
  `IteratorImpl`. `IteratorImpl::~IteratorImpl()` guards against
  null, but `RecordFirstNextInSqlStats()` does *not* — it
  unconditionally dereferences. The first call to `Iterator::Next()`
  on a secondary-connection iterator hung (effectively) due to
  cascaded UB. Fix: pass the parent through and use it for sql_stats
  bookkeeping. Future cleanup option: gate `RecordFirstNextInSqlStats`
  on a non-null check, but no current call-site needs that.
  Build/test results on `out/mac_release`: gn check clean, full
  unittests pass (3219 + 1 pre-existing skip
  `HttpServerTest.Websocket`; the new 3 connection tests are
  included), 122 TP integrationtests pass
  (`TraceProcessor*:*Sqlite*:ReadTrace*` filter), 1355 diff tests
  pass + 9 pre-existing skips. No behaviour change for existing
  callers.
- 2026-04-29 [Phase 2 iter 1]: tp-public-api-create-conn done.
  Public API surface added in
  `include/perfetto/trace_processor/trace_processor.h`:
  nested abstract `class TraceProcessor::Connection` (movable-via-
  pointer through `unique_ptr`, non-copyable; pure-virtual
  `Iterator ExecuteQuery(const std::string&)` matching the parent
  signature; out-of-line ctor/dtor in
  `src/trace_processor/trace_processor.cc`) and a new
  `virtual std::unique_ptr<Connection> CreateConnection() = 0;` on
  `TraceProcessor`. Connection-0 (`TraceProcessor::ExecuteQuery`
  directly) is preserved exactly.
  `TraceProcessorImpl::ConnectionImpl` is the Phase 2 iter 1
  scaffold: it stores a back-pointer and forwards `ExecuteQuery`
  to `TraceProcessorImpl::ExecuteQuery` (i.e. connection-0). It is
  NOT yet a real per-connection engine — that's the next chunk
  (`perfetto-sql-engine-per-conn`). The dtor calls
  `TraceProcessorImpl::ReleaseConnection` to decrement the live
  counter.
  Strict-v1 mutating-method gating via
  `PERFETTO_CHECK(non_default_connection_count_ == 0)` near the
  top of: `Parse`
  (`src/trace_processor/trace_processor_impl.cc:737`),
  `NotifyEndOfFile` (line ~748), `RegisterSqlPackage` (line ~833),
  `RegisterFileContent` (line ~999), `RestoreInitialTables`
  (line ~1011), `RegisterMetric` (line ~1043),
  `ExtendMetricsProto(skip_prefixes)` (line ~1103),
  `CreateSummarizer` (line ~1611). `CreateConnection` itself
  also `PERFETTO_CHECK`s `notify_eof_called_` (concurrent
  ingestion is out of scope per design rule). The
  `~TraceProcessorImpl` dtor `PERFETTO_CHECK`s the counter is 0.
  Note: `Flush`, `Summarize`, and `ComputeMetric*` are *not* gated
  — they are query-execution paths, not registration. If a future
  chunk discovers `Flush` mutates schema, gate it then.
  3216 unittests pass + 1 skipped (pre-existing
  `HttpServerTest.Websocket`), 122 TP integrationtests pass, 1355
  diff tests pass + 9 pre-existing skips. No behaviour change for
  existing callers.
- 2026-04-29 [iter 10]: phase1-validation done. Final clean sweep on
  `out/mac_release`: 3216 unittests pass + 1 skipped (only pre-existing
  `HttpServerTest.Websocket` failure on macOS, ignored), 122 TP
  integrationtests pass (filter
  `TraceProcessor*:*Sqlite*:ReadTrace*`), 1355 diff tests pass + 9
  skipped (etm + llvm_symbolizer modules absent — pre-existing). ASan
  pass on `out/mac_asan` (`is_clang=true is_asan=true is_debug=false`):
  built `perfetto_unittests` from scratch under ASan, then ran the
  TP/SQL-scoped filter
  `*Sql*:*Sqlite*:*Trace*Processor*:-HttpServerTest.Websocket` —
  123/123 tests pass with zero ASan reports (no leak / use-after-free /
  stack-use-after-scope). No source changes this iteration; only
  STATUS.md and the project memory file updated to record Phase 1
  completion. **Phase 1 closes here.**
- 2026-04-29 [iter 9]: phase1-pragmas done. `SqliteConnection`
  constructor now applies `journal_mode=MEMORY`, `temp_store=MEMORY`,
  and `locking_mode=NORMAL` after `InitializeSqlite`, then re-reads
  each pragma and `PERFETTO_CHECK`s the result so silent fallbacks
  crash loudly. Helpers `ReadPragma` and `ApplyAndVerifyPragma` live
  in the anonymous namespace in `sqlite_engine.cc`. Note:
  `PRAGMA temp_store` reads back as `2` (the integer encoding of
  `MEMORY`), so the verifier accepts both `"2"` and `"memory"` to be
  robust across SQLite versions; verified empirically on this build
  (no fallback CHECK fired across 3216 unittests + 122 TP integration
  tests + 1355 diff tests). `journal_mode` and `locking_mode` came
  back as the expected `memory`/`normal` strings — no surprises. The
  pre-existing `temp_store=2` exec inside `InitializeSqlite` was
  removed (subsumed by the new `temp_store=MEMORY` apply-and-verify).
  No behaviour change.
- 2026-04-29 [iter 7]: global-staging-area-skeleton done. Added
  `src/trace_processor/perfetto_sql/engine/global_staging_area.{h,cc}`
  declaring an empty `GlobalStagingArea` class (no state, copy/move
  deleted, ctor/dtor out-of-line so Phase 2 can add private members
  without rebuilds). Wired a `std::unique_ptr<GlobalStagingArea>
  staging_area_` member into `TraceProcessorImpl`, initialised in
  the constructor; no callsites yet. Added the new sources to the
  `engine` source set in `src/trace_processor/perfetto_sql/engine/
  BUILD.gn`. `gn check` clean. 3216 unittests pass (only pre-existing
  `HttpServerTest.Websocket` failure on macOS, ignored), 122 TP
  integrationtests pass, 1355 diff tests pass. No behaviour change.
- 2026-04-29 [iter 6]: connection-handle-struct done. Extracted
  `SqliteConnection` value-type (declared in `sqlite_engine.h` next
  to `SqliteEngine`) wrapping `ScopedDb db_` + the per-handle
  `fn_ctx_` map. Constructor takes the URI filename and opens the
  handle (calls `EnsureSqliteInitialized`, `sqlite3_open_v2`,
  `InitializeSqlite`); destructor drops registered functions before
  closing. `SqliteEngine` now holds `std::string filename_` (per-
  engine, shared) + a single `SqliteConnection connection_` value
  member; all public methods forward to `connection_`. Public API
  unchanged. 3216 unittests pass (only pre-existing
  `HttpServerTest.Websocket` failure on macOS, ignored), 122 TP
  integrationtests pass, 1355 diff tests pass. No behaviour change.
- 2026-04-29 [iter 5]: sqlite-handle-encapsulation done. Added
  `sqlite3* PerfettoSqlEngine::db()` (one-line accessor returning
  `engine_->db()` today; doc-commented as the Phase 2 swap point).
  Migrated 9 external callsites away from
  `engine_->sqlite_engine()->db()` to `engine_->db()`:
  `trace_processor_impl.cc` (4 sites: lines 762, 957, 960, 1044,
  1314), `perfetto_sql/intrinsics/operators/span_join_operator.cc`
  (1: line 230), `perfetto_sql/engine/created_function.cc`
  (3: lines 427, 571, 729), and
  `perfetto_sql/intrinsics/operators/span_join_operator_unittest.cc`
  (1: line 49). Internal `sqlite_engine()->db()` callers inside
  `perfetto_sql_engine.cc` were intentionally left as
  `engine_->db()` (already direct on the unique_ptr<SqliteEngine>
  member) — those *are* the engine internals. Verified: no remaining
  `sqlite_engine()->db()` callsites outside the docstring on the new
  accessor. 3216 unittests pass (only pre-existing
  `HttpServerTest.Websocket` failure on macOS, ignored), 122 TP
  integrationtests pass, 1355 diff tests pass. No behaviour change.
- 2026-04-29 [iter 4]: wal-mode-pragma **BLOCKED** — discovered the
  in-tree `memdb` VFS does not implement shared-memory hooks
  (`xShmMap`/`xShmLock`/`xShmBarrier`/`xShmUnmap` are all `0` in
  `buildtools/sqlite_src/src/memdb.c:175-179`). SQLite requires SHM for
  WAL, so `PRAGMA journal_mode=WAL` on a memdb-backed handle silently
  falls back to `journal_mode=memory`. Verified empirically: prototype
  CHECK on read-back of `journal_mode` fired on every TraceProcessor
  construction with the message *"PRAGMA journal_mode=WAL silently
  fell back to 'memory'"*. Reverted the change; no code committed this
  iteration. Path forward options for the parent loop to weigh:
  (a) ship a custom in-memory VFS with SHM support (non-trivial: ~few
  hundred LOC; SQLite's `test_multiplex.c` and the WASM `kvvfs` are
  precedent) and use that instead of `memdb`; (b) accept
  `journal_mode=memory` and rely solely on `cache=shared`'s table-level
  locking (kills the design's "readers concurrent with writer"
  promise — but per the project plan, Phase 2 is single-threaded
  multi-conn, so this is fine for Phases 1-2 and only blocks Phase 3);
  (c) defer WAL to Phase 3 and treat this chunk as superseded.
  Recommendation: (c) — drop `wal-mode-pragma` from Phase 1 chunks,
  add a Phase 3 chunk *custom-shm-vfs* to revisit. Phase 2's
  single-threaded multi-conn semantics work fine with `journal_mode=
  memory` + `cache=shared`.
- 2026-04-29 [iter 3]: memdb-vfs-open done. `SqliteEngine` ctor now
  opens `file:/perfetto-<N>?vfs=memdb&cache=shared` (N from a static
  `std::atomic<uint64_t>` counter, monotonically incremented per
  instance) with flags `READWRITE|CREATE|NOMUTEX|URI`. Filename stored
  as `std::string filename_` on `SqliteEngine` for later second-
  connection use; not exposed publicly. 3216 unittests pass (only
  pre-existing `HttpServerTest.Websocket` failure on macOS, ignored),
  122 TP integrationtests pass, 1355 diff tests pass. No behaviour
  change.
- 2026-04-29 [iter 2]: sqlite-build-flags done. Flipped
  `SQLITE_THREADSAFE=0`→`=2` and removed `-DSQLITE_OMIT_SHARED_CACHE`
  in `buildtools/BUILD.gn`. Audited remaining flags — all compatible
  with multi-conn. URI parsing not enabled globally; will rely on
  `SQLITE_OPEN_URI` per-open in next chunk. Builds + 1743 TP unittests
  + 122 TP integration tests + 1355 diff tests all pass on macOS.
  (Two unrelated pre-existing macOS failures observed:
  `HttpServerTest.Websocket` framing bug, and a tracing-service
  integration test that needs `@traced_relay` abstract socket — both
  pre-date this change and are independent of SQLite.)
- 2026-04-29 [iter 1]: scaffolded loop status doc, surveyed TP
  architecture, created branch `dev/lalitm/multi-conn-tp`. No code
  changes. Discovered SQLite is currently built with
  `SQLITE_THREADSAFE=0` and `SQLITE_OMIT_SHARED_CACHE` — both must be
  flipped before any multi-connection work can land. SQLite's built-in
  `memdb` VFS is already compiled in (see
  `buildtools/sqlite_src/src/memdb.c`); a shared in-memory database is
  obtainable via `file:/<name>?vfs=memdb&cache=shared` without writing
  a custom VFS.

## Phase 1 wrap-up

Branch: `dev/lalitm/multi-conn-tp` — 10 commits ahead of `main` (9 code
+ this validation commit).

What shipped:
- SQLite build flags flipped: `SQLITE_THREADSAFE=0`→`=2`,
  `-DSQLITE_OMIT_SHARED_CACHE` removed (`buildtools/BUILD.gn`).
- SQLite handle now opens via `file:/perfetto-<N>?vfs=memdb&cache=
  shared` instead of `:memory:` — same in-memory backing, but with a
  named URI so a future second connection can attach via shared cache.
- WAL deferred indefinitely after empirical confirmation that the
  in-tree `memdb` VFS lacks SHM hooks; replaced with serialized
  transactions (see pragmas below). Project memory and design pivots
  reflect this.
- `sqlite3*` access funneled through `PerfettoSqlEngine::db()` —
  9 external callsites migrated; raw `engine_->sqlite_engine()->db()`
  no longer leaks past the engine boundary.
- `SqliteConnection` value-type extracted from `SqliteEngine`,
  bundling `ScopedDb` + per-handle `fn_ctx_` map. `SqliteEngine` now
  holds `filename_` (per-engine) + one `SqliteConnection`; public API
  unchanged.
- `GlobalStagingArea` skeleton added at
  `src/trace_processor/perfetto_sql/engine/global_staging_area.{h,cc}`
  and owned by `TraceProcessorImpl` via `unique_ptr`. No state, no
  callers yet — fillable in Phase 2.
- `SqliteConnection` ctor now applies and read-back-verifies three
  pragmas: `journal_mode=MEMORY`, `temp_store=MEMORY`,
  `locking_mode=NORMAL`. Verifier accepts both `"2"` and `"memory"`
  for `temp_store` (SQLite reports the integer encoding). Silent
  fallbacks would `PERFETTO_CHECK`-crash; none observed across the
  full test suite.

No behaviour change observable to existing callers: `TraceProcessor`
public API is byte-identical, single-connection externally, all
3216 unittests + 122 TP integrationtests + 1355 diff tests green
(plus pre-existing macOS-only failure ignored), and the same set of
files passes ASan.

Phase 2 starts at the existing "Next chunks (Phase 2 — first cut...)"
section below: `tp-public-api-create-conn` is the first chunk.

## Phase 2 wrap-up

Branch: `dev/lalitm/multi-conn-tp` — 16 commits ahead of `main` (15 code
+ this iter-6 commit). Phase 2 closes here.

What shipped across iters 1-6:
- **Public API** (`tp-public-api-create-conn`, iter 1): nested
  `TraceProcessor::Connection` abstract class + virtual
  `CreateConnection()` on `TraceProcessor`. Mutating TP-level methods
  (`Parse`, `NotifyEndOfFile`, `RegisterSqlPackage`,
  `RegisterFileContent`, `RestoreInitialTables`, `RegisterMetric`,
  `ExtendMetricsProto`, `CreateSummarizer`) gated via
  `PERFETTO_CHECK(non_default_connection_count_ == 0)`.
- **Per-connection engine** (`perfetto-sql-engine-per-conn`, iter 2):
  each non-default `Connection` mints a fresh `PerfettoSqlEngine`
  sharing the primary's memdb URI via `cache=shared`. Filename-sharing
  via `SqliteEngine::filename()` accessor + alternate ctor.
- **Atomic includes** (`include-temp-then-promote`, iter 3):
  `SAVEPOINT perfetto_include_<n>` per `INCLUDE PERFETTO MODULE`
  (one per wildcard expansion too). Successful includes RELEASE so
  `cache=shared` propagates; failed includes ROLLBACK TO so partial
  installation does not leak. `GlobalStagingArea::AcquireIncludeLock`
  API added (not yet plumbed — Phase 3 hook).
- **Cross-connection vtab state** (`vtab-state-staging-publish`,
  iter 4): `GlobalStagingArea` owns a vtab-state map keyed by
  `(module + "\0" + vtab_name)`. `DataframeModule::Context` publishes
  on `OnCommit` and resolves on cold xConnect /
  `BestIndex` / `Filter` re-resolution. Static `thread`/`process`
  tables and `CREATE PERFETTO TABLE`-defined dataframes are now
  queryable from secondary connections.
  `RuntimeTableFunctionModule` and `StaticTableFunctionModule`
  remain deferred (engine-pointer-in-State and Cursor-sharing
  concerns).
- **Function pool diff** (`function-pool-per-conn-diff`, iter 5):
  `GlobalStagingArea` carries an additive
  `vector<FunctionPoolEntry>`. Writer appends after local register
  succeeds; readers replay via `SyncFunctionsFromPool` at the top of
  every top-level `Execute`. Stateless functions only — replays
  from prototype + return type + SqlSource.
- **Multi-statement atomicity** (`execute-savepoint-wrap`, iter 6):
  every top-level `ExecuteUntilLastStatement` opens
  `SAVEPOINT perfetto_execute_<n>` (RELEASE on success, ROLLBACK TO
  on error). Per-include savepoints from iter 3 nest cleanly inside.

Test counts at Phase 2 close vs. Phase 1: 3228 unittests (was 3216,
+12 new across iters 1-6) + 1 pre-existing skip
(`HttpServerTest.Websocket`); 122 TP integrationtests (unchanged);
1355 diff tests (unchanged) + 9 pre-existing skips (etm +
llvm_symbolizer modules absent). No regressions and no behaviour
change for single-connection callers — the public API surface is
additive-only.

What's deferred to Phase 3 (or beyond):
- Static built-in functions registered post-`InitPerfettoSqlEngine`
  on the writer don't replicate to readers (only dynamic
  CREATE-PERFETTO-FUNCTION entries land in the pool today). Out of
  scope until a real workload needs it.
- `RuntimeTableFunctionModule` cross-conn (engine pointer in State).
- `StaticTableFunctionModule` cross-conn (Cursor sharing).
- `GlobalStagingArea::AcquireIncludeLock` not yet plumbed into
  `PerfettoSqlEngine::IncludeModuleImpl`. Single-threaded today, so
  contention is impossible; the lock is the Phase 3 thread-safety
  hook.
- SQLITE_BUSY / SQLITE_SCHEMA retry middleware (Phase 3).
- TSan / multi-thread fan-out audit (Phase 3).

Phase 3 (`thread safety + retry middleware`) is the next loop entry
point: verify `SQLITE_THREADSAFE=2` is honoured under multi-thread
load, add transparent BUSY/SCHEMA retry with configurable timeout,
audit globals for thread-safety, stress-test under TSan/ASan.

## Next chunks (Phase 1)
- [x] sqlite-handle-encapsulation — done iter 5. Accessor:
      `sqlite3* PerfettoSqlEngine::db()`. 9 external callsites
      migrated. See iter 5 activity entry.
- [x] connection-handle-struct — done iter 6. `SqliteConnection`
      bundles `ScopedDb` + per-handle `fn_ctx_`; `SqliteEngine` keeps
      `filename_` and owns one `SqliteConnection` by value. See iter 6
      activity entry.
- [x] global-staging-area-skeleton — done iter 7. Empty
      `GlobalStagingArea` class added; `TraceProcessorImpl` owns it
      via `std::unique_ptr`. No callsites yet. See iter 7 activity
      entry.
- [x] phase1-pragmas — done iter 9. Three pragmas applied inside
      `SqliteConnection` ctor with read-back verification via
      `ApplyAndVerifyPragma` helper. `temp_store` reads back as `2`
      (integer form), other two as `memory`/`normal`. See iter 9
      activity entry.
- [ ] phase1-validation — run unittests, integrationtests,
      diff_test_trace_processor.py, and an ASan unittests pass. No
      regressions and no behaviour change vs. main. Mark Phase 1 done
      and append a Phase 1 wrap-up section to this file. Update the
      project memory file at
      `/Users/lalitm/.claude/projects/-Users-lalitm-perfetto/memory/project_multi_connection_tp.md`
      to record Phase 1 completion (note: design pivots already landed
      in memory file — verify those are consistent with what shipped).

## Next chunks (Phase 2 — first cut, refine on /loop restart)

These are **draft** — the orchestrator should re-read the design memo
and the temp-then-promote breakthrough above before sequencing them.

- [x] tp-public-api-create-conn — done Phase 2 iter 1. Public
      API surface (`TraceProcessor::Connection` + `CreateConnection`)
      added; mutating TP-level methods (`Parse`, `NotifyEndOfFile`,
      `RegisterSqlPackage`, `RegisterFileContent`,
      `RestoreInitialTables`, `RegisterMetric`,
      `ExtendMetricsProto`, `CreateSummarizer`) gated via
      `PERFETTO_CHECK(non_default_connection_count_ == 0)`. The
      Connection impl is currently a connection-0-shallow stub
      (forwards `ExecuteQuery` to `TraceProcessorImpl::ExecuteQuery`).
      Replacing the stub with a real per-conn engine is the next
      chunk. See Phase 2 iter 1 activity entry.
- [x] perfetto-sql-engine-per-conn — done Phase 2 iter 2. Each
      non-default `Connection` mints a fresh `PerfettoSqlEngine`
      sharing the primary's memdb URI via `cache=shared`. Three
      smoke tests under `TraceProcessorConnectionTest.*` verify
      basic SELECT, cross-conn schema visibility, and multiple
      live connections. Vtab/function registry on the secondary
      engine is intentionally empty — addressed by the next two
      chunks. See Phase 2 iter 2 activity entry.
- [x] include-temp-then-promote — done Phase 2 iter 3.
      Implemented as Option C (SAVEPOINT-per-include) rather than
      literal `temp.<name>` rewriting; for plain SQL DDL the
      RELEASE+`cache=shared` path achieves the same cross-conn
      promotion. Wildcard expansion gets one savepoint per
      module. Failed includes ROLLBACK TO; successful includes
      RELEASE. `GlobalStagingArea::AcquireIncludeLock` API
      added but not yet wired into `PerfettoSqlEngine`
      (Phase 3 concern). Three tests under
      `TraceProcessorConnectionTest.*` cover promotion,
      atomicity-on-failure, and sequential includes. See
      Phase 2 iter 3 activity entry for deferred TODOs.
- [x] vtab-state-staging-publish — done Phase 2 iter 4. Dataframe
      vtab module is wired: `OnCommit` publishes to staging, cold
      xConnect on a reader connection reads from staging,
      `BestIndex`/`Filter` re-resolve from staging at query time
      (no caching in `PerVtabState`). Two new tests under
      `TraceProcessorConnectionTest.*` verify a secondary
      connection can SELECT both a CREATE-PERFETTO-TABLE-defined
      vtab and a static dataframe (`thread`) registered during
      engine init. **Deferred**:
      `RuntimeTableFunctionModule` (engine pointer in State is
      cross-conn incoherent — needs design work) and
      `StaticTableFunctionModule` (mechanical follow-on, just
      not done in this chunk). See Phase 2 iter 4 activity entry.
- [x] function-pool-per-conn-diff — done Phase 2 iter 5.
      `GlobalStagingArea` carries an additive `vector<FunctionPoolEntry>`
      keyed by insertion order, with `AppendFunction` (writer-only) /
      `SnapshotSince` / `LatestFunctionVersion` / `ResetFunctionPool`.
      `PerfettoSqlEngine` split `RegisterLegacyRuntimeFunction` into
      a local helper + writer-side wrapper that appends after the
      local register succeeds; `SyncFunctionsFromPool` runs at the
      top of every top-level `ExecuteUntilLastStatement`. Writers
      short-circuit (they're the source of truth); readers iterate
      `snapshot.entries` and replay via the local helper.
      `RestoreInitialTables` calls `ResetFunctionPool` before
      rebuilding the writer engine. Two tests cover propagation and
      incremental pickup. See Phase 2 iter 5 activity entry for
      deferred follow-ons (static built-ins, runtime table functions).
- [x] execute-savepoint-wrap — done Phase 2 iter 6. Every top-level
      `ExecuteUntilLastStatement` (the funnel point all `Execute`
      callers and `IteratorImpl` go through) opens
      `SAVEPOINT perfetto_execute_<n>` immediately after the
      function-pool sync and either RELEASEs (success) or ROLLBACK TOs
      (error) before returning. Gated on `stack_base == 0` so
      re-entrant `Execute` calls from statement handlers don't double-
      wrap. Two tests verify rollback-on-failure (including
      cross-connection visibility check) and commit-on-success. See
      Phase 2 iter 6 activity entry.

## Next chunks (Phase 3 — first cut, refine on /loop restart)

- [x] include-lock-wire-and-mt-smoke — done Phase 3 iter 1.
      `GlobalStagingArea::AcquireIncludeLock` is now wired into the
      include path on the `ExecutionFrame`; per-module mutex switched to
      `std::recursive_mutex` so re-entrant include of the same module
      doesn't self-deadlock. Two tests added under
      `TraceProcessorConnectionTest.*`: a multi-thread reader smoke and
      a single-thread include-lock-acquire-without-deadlock. See
      Phase 3 iter 1 activity entry.
- [x] cross-conn-package-propagation — done Phase 3 iter 2.
      `GlobalStagingArea` grows an additive package pool mirroring the
      function pool from iter 5; writer's `RegisterSqlPackage` appends
      after successful local register, readers diff
      `last_synced_package_version_` at the top of every top-level
      `ExecuteUntilLastStatement` and locally `RegisterPackageLocal`
      missing entries. Pool entries store raw (module-name, sql) pairs
      in a `shared_ptr` because `RegisteredPackage` is move-only.
      `RestoreInitialTables` resets the pool. Also wired a
      cross-connection `IsModuleIncluded` set so the second connection
      to include the same module short-circuits before the body
      (previous attempt would collide with shared-`main` schema).
      Three new tests; the naive concurrent-include variant (without
      pre-include) fails on SQLITE_LOCKED — that's busy-retry
      territory.
- [ ] busy-retry — transparent `SQLITE_BUSY` retry middleware with
      bounded retry count + configurable timeout (default 1s). Wraps
      `PrepareStatement` / `Step`. Required once concurrent writers
      become possible (today's secondary connections are read-only in
      practice).
- [ ] schema-retry — transparent `SQLITE_SCHEMA` retry: re-prepare the
      statement when the schema cookie changes underneath an in-flight
      query. Drops in alongside busy-retry on the same wrapper.
- [x] globals-audit — done Phase 3 iter 3. Headline fix:
      `TraceStorage::SqlStats` had un-guarded
      `std::deque<string>::push_back` from concurrent connection
      threads (the iter-2 ASan flake). Wrapped all writers in a
      `mutable std::mutex` and replaced the raw `const deque&` read
      accessors with a `SnapshotForReading()` returning a copy under
      the lock; `SqlStatsModule::Cursor` carries the snapshot for
      iteration. Audit pass also flagged `StringPool` (next chunk)
      and confirmed `TraceStorage` data tables are race-free by the
      "multi-conn legal only post-EOF" gate. New stress test
      `ConcurrentRecordingIntoSqlStats` (4 threads, 400 queries) is
      clean across 10 consecutive ASan runs.
- [ ] string-pool-thread-safety — flip
      `StringPool::set_locking(true)` on the `TraceStorage`-owned
      `string_pool_` (and any pool reachable from query handlers)
      whenever multi-conn becomes possible, and / or replace the
      `MaybeLockGuard` toggle with always-on locking now that
      `Get(Id)` is already lock-free. Surfaced by the Phase 3 iter 3
      audit pass: the hashtable insertion path
      (`string_index_.Insert` + `InsertString`) is unguarded today
      because `should_acquire_mutex_` defaults to `false` and no
      production code flips it on, but query-side handlers like
      `GProfileBuilder::StringTable::InternString`, `json_args`'s
      `storage->InternString`, and `RuntimeDataframeBuilder` all
      intern at query time. Quiescent today (single-conn writers
      only) but a guaranteed race once Phase 4 fans queries across
      threads.
- [ ] tsan-multithread-stress — bring up a TSan-enabled build (or
      document the toolchain-availability blocker on macOS) and add a
      larger fan-out test: writer + multiple readers running interleaved
      DDL + SELECT. Goal: prove the data-race-free invariant under
      stress, not just the smoke baseline from iter 1.

## Architecture notes (for future iterations)
- Public API:
  `include/perfetto/trace_processor/trace_processor.h:90`
  declares `virtual Iterator ExecuteQuery(const std::string&) = 0;`.
- Ownership chain: `TraceProcessor`
  → `TraceProcessorImpl` (`src/trace_processor/trace_processor_impl.h:53`)
  → `std::unique_ptr<PerfettoSqlEngine> engine_` (line 182)
  → `std::unique_ptr<SqliteEngine> engine_`
    (`src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h:441`)
  → `ScopedDb db_` wrapping `sqlite3*`.
- ExecuteQuery flow:
  `TraceProcessorImpl::ExecuteQuery`
  (`trace_processor_impl.cc:769`) → `PerfettoSqlEngine::ExecuteUntilLastStatement`
  (`perfetto_sql_engine.cc:531`) → `ExecuteUntilLastStatementImpl`
  (line 722) → `SqliteEngine::PrepareStatement`
  (`sqlite_engine.cc:136`) which calls `sqlite3_prepare_v2` on `db_`.
  `IteratorImpl` then drives `sqlite3_step` per `Next()`.
- SQLite handle creation lives in `SqliteEngine::SqliteEngine()`
  (`sqlite_engine.cc:105`). It opens `:memory:` with
  `SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX`,
  then calls `InitializeSqlite(db)` (defined in
  `src/trace_processor/sqlite/bindings/`).
- No custom VFS is currently registered by TP. SQLite's in-tree
  `memdb` VFS is compiled in via `buildtools/sqlite_src/src/memdb.c`
  (gated on `!SQLITE_OMIT_DESERIALIZE`, which is not defined). Shared
  memdb files use URIs of the form `file:/<name>?vfs=memdb`; first
  char of name must be `/`.
- **memdb does NOT support WAL** (kept here for the next person who
  tries). `buildtools/sqlite_src/src/memdb.c` registers `xShmMap=0,
  xShmLock=0, xShmBarrier=0, xShmUnmap=0` (lines 176-179). SQLite's
  `pager_open_journal` requires shared-memory for WAL, so `PRAGMA
  journal_mode=WAL` against a memdb handle returns `memory` (silent
  fallback). **The project no longer needs WAL** — see Design pivots
  above; concurrency comes from serialized writes, not WAL. Don't
  reopen this rabbit hole.
- SQLite compile flags in `buildtools/BUILD.gn:1664-1685`. Two are
  load-bearing for this project:
  - `-DSQLITE_THREADSAFE=0` — must move to `=2` (multi-thread, no
    serialised mutexes) before Phase 3. Even Phase 2 (multi-conn,
    single-thread) is safer with `=2` so we can rule out latent
    threading assumptions.
  - `-DSQLITE_OMIT_SHARED_CACHE` — **blocks** `cache=shared`. Must be
    removed. Schema sharing across connections relies on this.
- `PerfettoSqlEngine` already tracks
  `std::vector<sqlite::ModuleStateManagerBase*> virtual_module_state_managers_`
  (perfetto_sql_engine.h:422) and calls `OnCommit`/`OnRollback`
  on each (perfetto_sql_engine.cc:1394, 1401). With the
  temp-then-promote pivot, this is now mostly relevant for
  *runtime* state (data) rather than schema: schema lands in main
  via DDL re-issue, and `cache=shared` propagates it. Vtab state
  staging via `GlobalStagingArea` still applies for data (e.g.
  dataframe handles).
- `ModuleStateManagerBase::PerVtabState`
  (`src/trace_processor/sqlite/module_state_manager.h:35`) already has
  separate `active_state` / `committed_state` / `savepoint_states`
  fields — perfect shape for cross-connection publishing of vtab
  *data* (the schema side is handled by temp-then-promote DDL).
- `DataframeModule::State` (`dataframe_module.h:48`) holds a raw
  `dataframe::Dataframe*` plus `std::unique_ptr<dataframe::Dataframe>
  owned_dataframe`. Per the design rule, the dataframe vtab will
  re-resolve from staging at cursor creation; no caching needed in
  `PerVtabState`.
- `RestoreInitialTables`
  (`trace_processor_impl.cc:966`) currently destroys and reinitialises
  `engine_`. With multi-conn, this must invalidate non-default
  connections too.
- Mutating TP methods to gate behind "non-default conn alive":
  `Parse`, `NotifyEndOfFile`, `RegisterSqlPackage`,
  `RegisterFileContent`, `RestoreInitialTables`, the v1/v2 metric
  registration functions, and the summarizer create path.
- Phase 2 raw-handle dispatch (`PerfettoSqlEngine::db()`) — most of
  the 9 funneled callsites are connection-scoped (one connection,
  one handle) and trivially work with per-connection dispatch. Two
  warrant attention:
  - `TraceProcessorImpl::InterruptQuery` (line 957/960) calls
    `sqlite3_interrupt` on the default connection's handle. With
    multi-conn, "interrupt" semantics need to fan out across all
    connections (or the API needs a connection-id param). For now
    the funneled accessor returns the default conn's handle, which
    matches today's semantics.
  - `TraceProcessorImpl::CreateEngine` line 1314
    (`sqlite3_str_split_init(engine->db())`) registers a SQLite
    function on the handle. In multi-conn this needs to be per-
    connection (or hoisted to the function pool). The funneled
    accessor preserves today's "register on default conn" behaviour.

## Phase plan
- **Phase 1 (current, almost done): refactor + memdb (serialized
  transactions, no WAL), no behaviour change.** See "Next chunks"
  above. Outcome: TP still presents a single connection externally,
  but internally is opened via shared-memdb with
  `journal_mode=MEMORY` + `temp_store=MEMORY` +
  `locking_mode=NORMAL`, and the raw `sqlite3*` handle is
  encapsulated behind a small surface, with an empty
  `GlobalStagingArea` ready to be filled.
- **Phase 2: multi-conn single-threaded.** Add
  `CreateConnection`/`DestroyConnection` to `TraceProcessor`.
  Implement the connection class as
  `{PerfettoSqlEngine, ModuleStateManager, last_synced_version_}`.
  Implement the temp-then-promote include pattern as the primary
  cross-connection schema-sync mechanism. Wire
  `GlobalStagingArea` for vtab *data* state (publish on `OnCommit`,
  read on cold xConnect) and the function pool (per-conn diff-and-
  register at `Execute` start). Add per-module include locks.
  Connection-0 keeps the existing API untouched. Gate mutating
  TP-level methods. Add savepoint-wrapping to `Execute`.
- **Phase 3: thread safety + retry middleware.** Verify
  `SQLITE_THREADSAFE=2` (already flipped in iter 2). Add
  transparent `SQLITE_BUSY` and `SQLITE_SCHEMA` retry with
  configurable timeout (default 1s). Audit all globals for
  thread-safety. Stress-test multi-thread fan-out with sanitizers
  (TSan, ASan). **WAL is no longer in scope** — concurrency comes
  from serialized writes per connection (one writer at a time
  across the shared cache, but multiple readers fine).
- **Phase 4: RPC pool + UI fan-out + WASM pthreads.** Add
  work-stealing thread pool sized to `#cpus` in the RPC layer.
  Connection pool is unbounded; each query: acquire conn, run, bulk
  materialise rows, release, stream buffer back. UI engine.ts fans
  queries out across pool. WASM build uses pthreads where COOP+COEP
  is available, with single-thread fallback.
