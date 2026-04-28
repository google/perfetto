# Multi-connection TraceProcessor — Loop Status

## Current Phase
Phase 1: refactor + memdb/WAL with no behavioural change

## Recent activity (newest first)
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

## Next chunks (Phase 1)
- [~] wal-mode-pragma — **BLOCKED, deferred to Phase 3.** memdb VFS
      lacks SHM hooks; WAL silently falls back to `memory`. See iter 4
      activity entry. Phase 1+2 will operate with `journal_mode=memory`
      + `cache=shared`, which is sufficient for single-threaded
      multi-conn. Replaced by a Phase 3 chunk: see *custom-shm-vfs*
      below.
- [ ] sqlite-handle-encapsulation — audit every direct
      `engine_->sqlite_engine()->db()` callsite in
      `trace_processor_impl.cc` (lines 762, 957, 960, 1044, ...) and
      `perfetto_sql_engine.cc`. Funnel raw-handle access through a
      single accessor on `PerfettoSqlEngine` so Phase 2 can swap the
      backing handle behind it without touching every callsite.
- [ ] connection-handle-struct — introduce a small
      `SqliteConnection` value-type wrapping `ScopedDb` plus the
      shared filename, used by `SqliteEngine`. Single-handle for now;
      sets up Phase 2 where multiple are minted from the same VFS.
- [ ] global-staging-area-skeleton — add
      `src/trace_processor/perfetto_sql/engine/global_staging_area.{h,cc}`
      as an empty class owned by `TraceProcessorImpl`. No state yet,
      no behaviour change, no callsites yet. Pure scaffolding so
      Phase 2 can fill in vtab-state map + function pool + per-module
      include locks.
- [ ] phase1-validation — run unittests, integrationtests,
      diff_test_trace_processor.py, and an ASan unittests pass. No
      regressions and no behaviour change vs. main.

## Phase 3 chunks (forward-deferred from Phase 1)
- [ ] custom-shm-vfs — write a small in-memory VFS that implements
      `xShmMap`/`xShmLock`/`xShmBarrier`/`xShmUnmap` so that WAL mode
      can be enabled. Without this, multi-threaded reads will serialise
      against any writer (table-level locks via `cache=shared` only).
      Reference: `buildtools/sqlite_src/src/memdb.c` (no SHM impl) and
      SQLite's `os_unix.c` SHM code as precedent. Likely ~300-500 LOC.
      Only needed once Phase 3 brings real concurrency.

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
- **memdb does NOT support WAL.** `buildtools/sqlite_src/src/memdb.c`
  registers `xShmMap=0, xShmLock=0, xShmBarrier=0, xShmUnmap=0`
  (lines 176-179). SQLite's `pager_open_journal` requires shared-memory
  for WAL, so `PRAGMA journal_mode=WAL` against a memdb handle returns
  `memory` (silent fallback). Cross-connection schema sharing via
  `cache=shared` still works without WAL — multiple handles see the
  same tables — but writes serialise against readers (table-level
  shared-cache locks). For Phase 1-2 (single-threaded multi-conn) this
  is acceptable. For Phase 3 (true concurrent reads with a writer) we
  must either ship a custom SHM-capable VFS (see *custom-shm-vfs*) or
  accept that writes block readers via shared-cache locks.
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
  on each (perfetto_sql_engine.cc:1394, 1401). This is the hook the
  `GlobalStagingArea` will plug into in Phase 2: `OnCommit` publishes
  to global, cold xConnect on connection B reads from global.
- `ModuleStateManagerBase::PerVtabState`
  (`src/trace_processor/sqlite/module_state_manager.h:35`) already has
  separate `active_state` / `committed_state` / `savepoint_states`
  fields — perfect shape for cross-connection publishing.
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

## Phase plan
- **Phase 1 (current): refactor + memdb (no WAL), no behaviour
  change.** See "Next chunks" above. Outcome: TP still presents a
  single connection externally, but internally is opened via
  shared-memdb (journal_mode=memory; WAL deferred — see iter 4) and
  the raw `sqlite3*` handle is encapsulated behind a small surface,
  with an empty `GlobalStagingArea` ready to be filled.
- **Phase 2: multi-conn single-threaded.** Add
  `CreateConnection`/`DestroyConnection` to `TraceProcessor`.
  Implement the connection class as
  `{PerfettoSqlEngine, ModuleStateManager, last_synced_version_}`.
  Wire `GlobalStagingArea` for vtab state (publish on `OnCommit`,
  read on cold xConnect) and the function pool (per-conn diff-and-
  register at `Execute` start). Add per-module include locks.
  Connection-0 keeps the existing API untouched. Gate mutating
  TP-level methods. Add savepoint-wrapping to `Execute`.
- **Phase 3: thread safety + retry middleware.** Bump
  `SQLITE_THREADSAFE=2` if not already done. Land *custom-shm-vfs*
  (or accept shared-cache table locks if measurements show it's good
  enough). Add transparent `SQLITE_BUSY` and `SQLITE_SCHEMA` retry
  with configurable timeout (default 1s). Audit all globals for
  thread-safety. Stress-test multi-thread fan-out with sanitizers
  (TSan, ASan).
- **Phase 4: RPC pool + UI fan-out + WASM pthreads.** Add
  work-stealing thread pool sized to `#cpus` in the RPC layer.
  Connection pool is unbounded; each query: acquire conn, run, bulk
  materialise rows, release, stream buffer back. UI engine.ts fans
  queries out across pool. WASM build uses pthreads where COOP+COEP
  is available, with single-thread fallback.
