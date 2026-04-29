# Multi-connection TraceProcessor — Loop Status

## Current Phase
Phase 1: refactor + memdb (serialized transactions, no WAL) with no
behavioural change. **Almost done — only `phase1-pragmas` and
`phase1-validation` remain.**

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
- [ ] phase1-pragmas — set `PRAGMA journal_mode=MEMORY`,
      `PRAGMA temp_store=MEMORY`, `PRAGMA locking_mode=NORMAL` after
      open. Verify each pragma read-back returns the expected value
      (`memory`/`memory`/`normal`) — silent fallbacks must trigger a
      CHECK so future regressions are caught. Replaces the
      now-superseded `wal-mode-pragma` chunk. The pragmas should be
      applied inside `SqliteConnection`'s constructor so every minted
      connection gets them, not just the first. (`temp_store=MEMORY` is
      arguably moot since memdb is in-memory anyway, but it's defensive
      and cheap. `locking_mode=NORMAL` is the SQLite default; we set it
      explicitly so the intent is clear and it can't drift to
      `EXCLUSIVE` later — `EXCLUSIVE` would break `cache=shared`.)
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

- [ ] tp-public-api-create-conn — extend the `TraceProcessor`
      public API with `CreateConnection` / `DestroyConnection` (or
      pick the names from the design memo). Connection-0 stays the
      default and preserves `ExecuteQuery(sql)` behaviour exactly.
      Returns a `Connection` handle wrapping a per-conn
      `PerfettoSqlEngine`. Mutating TP methods (`Parse`,
      `NotifyEndOfFile`, `RegisterSqlPackage`,
      `RegisterFileContent`, `RestoreInitialTables`, metric/
      summarizer registration) should `PERFETTO_CHECK` that no
      non-default conn is alive — strict for v1 per design rule.
- [ ] perfetto-sql-engine-per-conn — the second connection mints a
      fresh `PerfettoSqlEngine` (and its own `SqliteEngine` /
      `SqliteConnection`) but reuses the existing `filename_` so
      `cache=shared` ties them together. Verify trivial query on
      conn 1 sees tables created on conn 0.
- [ ] include-temp-then-promote — implement the include
      breakthrough. Hijack the include execution to write CREATE
      DDL into `temp.<symbol>` first, then on success drop the temp
      versions and re-issue the DDL on main. Failure path: drop
      the temp objects, leave main untouched. Per-module include
      lock from `GlobalStagingArea` serialises concurrent imports
      of the same module. This is the *primary* cross-connection
      schema-sync mechanism — vet end-to-end before moving on.
- [ ] vtab-state-staging-publish — vtab `OnCommit` writes
      committed state into `GlobalStagingArea`'s vtab-state map;
      `OnRollback` discards. Cold xConnect on conn B pulls from
      global. Dataframe vtab re-resolves dataframe at cursor
      creation; no caching/invalidation in `PerVtabState`. (May
      partially overlap with include-temp-then-promote — sequence
      after that lands so we can see what's still load-bearing.)
- [ ] function-pool-per-conn-diff — `GlobalStagingArea` holds an
      additive-only function pool. Each conn tracks
      `last_synced_version_`; at `Execute` start, diff against pool
      and register missing entries on its own `sqlite3*`. No
      cross-thread sqlite mutation. Functions are stateless
      (SQL/fn pointer + signature) — no DROP semantics.
- [ ] execute-savepoint-wrap — every top-level
      `Execute(sql)` wraps in a savepoint for multi-statement
      atomicity. Fits naturally with the temp-then-promote include
      pattern.

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
