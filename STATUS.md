# Multi-connection TraceProcessor — Loop Status

## Current Phase
Phase 4 complete (with caveats — see wrap-up). Initiative is
feature-complete; visible parallelism is gated on a BtShared
follow-on (Phase 5 candidate: dataframe-only post-EOF query path).

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
- 2026-04-29 [Phase 4 iter 6 — wrap-up]: closing the loop.
  Documentation-only iter. Re-ran the full validation pass at
  branch tip (counts recorded in the new "Phase 4 wrap-up"
  section at the bottom of this file) and re-ran the ASan +
  TSan stress sweeps on `TraceProcessorConnectionTest.*` +
  `RpcTest.*` (clean). Phase 4 is feature-complete on the
  infrastructure axis but realised UI speedup is capped at
  ~1.02x by the iter-4 BtShared finding; see "Phase 4 wrap-up"
  below for the headline numbers, the three follow-on options,
  and the recommended Phase 5 entry point (dataframe-only
  post-EOF query path). No further code chunks in this iter —
  the only artifact is this entry, the new wrap-up section,
  and the updated project memory.

- 2026-04-29 [Phase 4 iter 5]: wasm-pthreads done. The wasm
  trace_processor build now ships a pthreads-enabled variant
  alongside the existing single-thread one, and the UI's
  `wasm_bridge.ts` runtime-selects between them based on
  `crossOriginIsolated`. Both Pieces (build infrastructure and
  runtime fallback) landed cleanly. **Caveat: speedup is gated on
  the iter-4 BtShared finding** — the pool *can* now spawn worker
  threads inside the wasm sandbox, but the post-EOF query flurry
  in the UI still hits SQLite's shared-cache btree mutex on
  trace-table reads. This iter lands the infrastructure; the
  realised speedup on real trace loads remains 1.02x until the
  shared-cache bottleneck is addressed (Phase 5 candidate).

  **Piece A — pthreads wasm build.**
  - `gn/wasm_vars.gni`, `gn/standalone/toolchain/BUILD.gn`: a
    new `wasm_pthreads` toolchain mirrors the existing
    `wasm`/`wasm_memory64` pattern. `is_wasm_pthreads` is true
    iff the current toolchain is `wasm_pthreads`.
  - `gn/standalone/BUILD.gn`: when `is_wasm_pthreads`, the
    standalone config injects `-pthread` into the compiler
    cflags. This is required at compile time so the .o files
    are emitted with the wasm `atomics` + `bulk-memory`
    features that the `--shared-memory` link flag needs.
  - `gn/standalone/wasm.gni`: `wasm_lib` gains an
    `enable_pthreads` parameter. When set, the executable
    target is routed to the new toolchain (so the dependency
    `:lib` is rebuilt with `-pthread`) and the link line picks
    up `-pthread -s PTHREAD_POOL_SIZE=8`. PTHREAD_POOL_SIZE
    pre-spawns 8 worker threads at module init so the
    base::ThreadPool dispatch in `rpc.cc` doesn't pay a
    cold-start round trip per task. Asserted that
    `is_memory64 + enable_pthreads` is rejected — emscripten
    doesn't support that combination today.
  - `src/trace_processor/BUILD.gn`: a third `wasm_lib`
    invocation (`trace_processor_pthreads_wasm`, output name
    `trace_processor_pthreads`) sits alongside
    `trace_processor_wasm` and `trace_processor_memory64_wasm`.
    All three share the same `trace_processor_wasm_deps` list.
  - **Artifact verified.** `tools/ninja -C out/ui
    trace_processor_pthreads_wasm` produces
    `out/ui/wasm_pthreads/trace_processor_pthreads.{js,wasm,d.ts}`.
    `.wasm` size is ~12.8 MB (matches the single-thread
    variant; same C++ payload, different wasm feature set).
    `.js` is 212 KB (vs 174 KB for the single-thread; the
    delta is emscripten's pthread bootstrap code that
    initialises a worker pool from a Blob URL). Emscripten's
    `MODULARIZE=1 + -pthread` combination embeds the worker
    script as a base64 Blob inside the .js, so there's no
    separate `.worker.mjs` file to chase through the build
    pipeline. Single-thread build (`trace_processor_wasm`) and
    memory64 build (`trace_processor_memory64_wasm`) still
    work — both rebuild cleanly after the gn changes.

  **Piece B — runtime fallback in the UI.**
  - `ui/src/engine/trace_processor_pthreads_stub.ts`: new
    stub file that throws if reached. Mirrors the existing
    `trace_processor_32_stub.ts` pattern.
  - `ui/config/rollup.config.js`: when not building
    `--only-wasm-memory64`, rollup rewrites the
    `./trace_processor_pthreads_stub` import to
    `../gen/trace_processor_pthreads`, the same way it
    handles the 32-bit stub. The replace pattern fires on
    every bundle (frontend, engine, traceconv,
    chrome_extension) — engine is the only one that uses the
    stub, but unconditionally rewriting is harmless and
    matches precedent.
  - `ui/src/engine/wasm_bridge.ts`: the constructor's module
    selector is now a 3-way branch — memory64 → 64-bit
    module; else, if `hasPthreadsSupport()` →
    `TraceProcessorPthreads`; else →
    `TraceProcessor32`. The new `hasPthreadsSupport()` helper
    returns `true` iff `self.crossOriginIsolated === true`
    *and* `typeof SharedArrayBuffer === 'function'`. The
    `crossOriginIsolated` global is propagated from the
    document into dedicated workers (which is where
    `engine/index.ts` runs), so this check works without an
    explicit message from the main thread. Memory64 hosts
    keep the single-thread path because emscripten doesn't
    support memory64+pthreads (the gn assert above fires at
    build time too).
  - `ui/build.js`: `cfg.wasmModules` now includes
    `trace_processor_pthreads` when not in
    `--only-wasm-memory64` mode. The wasm-output-dir
    selector grew a third arm: a module name ending in
    `_pthreads` reads from `wasm_pthreads/` in the gn out
    dir. The .js / .d.ts go to `out/ui/tsc/gen/` for the
    bundler; the .wasm goes to `out/ui/dist_version/`.

  **Deployment dependency.**
  - `ui.perfetto.dev` does **not** currently set COOP+COEP.
    Verified by reading
    `infra/ui.perfetto.dev/appengine/main.py`: the response
    headers passed through from GCS are `Content-Type`,
    `Content-Encoding`, `Content-Length`, `Cache-Control`,
    `Date`, `ETag`, `Last-Modified`, `Expires` — no
    `Cross-Origin-Opener-Policy` or
    `Cross-Origin-Embedder-Policy`. So in production today,
    `crossOriginIsolated` is `false`, and `wasm_bridge.ts`
    falls through to the single-thread variant. The pthreads
    module is built and shipped but never loaded.
  - Activating the pthreads path requires a separate
    deployment-side change to the appengine flask handler:
    add `Cross-Origin-Opener-Policy: same-origin` and
    `Cross-Origin-Embedder-Policy: require-corp` to the
    response. Note that flipping COEP requires every
    cross-origin subresource to carry `Cross-Origin-Resource-
    Policy: cross-origin`, so it's a non-trivial deployment
    change that's deliberately not bundled here.
  - The dev server (`ui/build`) already has a
    `--cross-origin-isolation` flag (build.js:306) that adds
    the headers; engineers can use it to manually exercise
    the pthreads path locally without deployment changes.

  **Validation.**
  - `tools/gn gen --check out/ui`: clean (1999 → 2416
    targets; the +417 is the new `wasm_pthreads` toolchain's
    targets).
  - `tools/gn check out/mac_release`: clean (host-side, no
    regression).
  - `tools/ninja -C out/ui trace_processor_pthreads_wasm`:
    builds clean. `tools/ninja -C out/ui trace_processor_wasm`
    and `trace_processor_memory64_wasm`: still build clean.
    Full `ui/build --typecheck` triggers all three wasm
    builds + the copy step into `dist_version/` and finishes
    without error.
  - `tools/ninja -C out/mac_release perfetto_unittests`: no
    rebuild required (the gn changes only affect the wasm
    toolchain).
  - `out/mac_release/perfetto_unittests --gtest_brief=1`:
    3269 PASSED, 2 SKIPPED, 1 pre-existing failure
    (`HttpServerTest.Websocket`, matches iter 4 baseline).
  - `ui/build --typecheck --no-depscheck`: clean (`tsc
    --project ../../ui --noEmit` exits 0; same for the
    service_worker project).
  - `ui/run-unittests --no-depscheck`: 125 suites, 2249
    passed, 1 skipped. Matches baseline.

  **Files touched.**
  - `gn/wasm_vars.gni` — declare `wasm_pthreads_toolchain`
    + `is_wasm_pthreads`; widen `is_wasm` and
    `is_wasm_memory32` to include the new toolchain.
  - `gn/standalone/toolchain/BUILD.gn` — instantiate the
    `wasm_pthreads` toolchain.
  - `gn/standalone/BUILD.gn` — add `cflags += [ "-pthread"
    ]` under `is_wasm_pthreads`.
  - `gn/standalone/wasm.gni` — `wasm_lib` gains
    `enable_pthreads`; pthreads ldflags + toolchain
    routing in the `group()` step.
  - `src/trace_processor/BUILD.gn` — third `wasm_lib`
    invocation for the pthreads variant.
  - `ui/build.js` — wire `trace_processor_pthreads` into
    `cfg.wasmModules` and the wasm-out-dir selector.
  - `ui/config/rollup.config.js` — rollup rewrite for
    `./trace_processor_pthreads_stub`.
  - `ui/src/engine/trace_processor_pthreads_stub.ts` — new
    stub file.
  - `ui/src/engine/wasm_bridge.ts` — 3-way module selector
    + `hasPthreadsSupport()` helper.
  - `STATUS.md` — this entry, ticking `wasm-pthreads`.

  **Recommendation.** Phase 4 is now feature-complete on
  the infrastructure axis: rpc-thread-pool (iter 1) +
  ui-engine-fan-out audit (iter 2) + httpd-pool-dispatch
  (iter 3) + e2e-perf-validation (iter 4) +
  wasm-pthreads (iter 5). Next: a Phase 4 wrap entry that
  consolidates the BtShared finding as the v1 ceiling, lists
  the deferred items (deployment-side COOP+COEP flip,
  dataframe-only post-EOF query path as a Phase 5
  candidate, RuntimeTableFunctionModule cross-conn,
  static-built-in fn replication, TSan-on-Linux), and ties
  the loop off. No further code chunks before the wrap.

- 2026-04-29 [Phase 4 iter 4]: e2e-perf-validation done. Built a
  Google-Benchmark harness that drives an end-to-end RPC-streaming
  query workload through `Rpc::OnRpcRequest` and measures wall-time
  with vs. without the iter-3 async dispatch path. The headline
  finding is **the pool *does* parallelise** (the dispatch wiring is
  correct; workers fan out to 8 threads on a 10-core machine and
  return a clean ~5x speedup on a CPU-bound workload that touches no
  shared trace tables) — but the trace-table workload that
  approximates the UI's post-EOF query flurry sees **no measurable
  speedup**. Investigation localised the bottleneck to SQLite's
  shared-cache `BtShared` mutex: with `cache=shared` (load-bearing
  for cross-connection schema sharing), every cursor read on a
  trace-table btree takes a per-database mutex that serialises
  reads across connections. This is a known SQLite limitation, not
  a bug in this PR's pool design. Surfacing as a **major project
  finding** for the project memo and the next iter.

  **Harness.**
  - File: `src/trace_processor/rpc/rpc_perf_benchmark.cc:1`.
  - Build: `tools/ninja -C out/mac_release perfetto_benchmarks`.
  - Run: `out/mac_release/perfetto_benchmarks
    --benchmark_filter='BM_Rpc.*' --benchmark_repetitions=10
    --benchmark_report_aggregates_only=true`.
  - 4 benchmarks, 2 pairs:
    - `BM_RpcStreamingQueryBurst_{PoolOn,PoolOff}` — 12 GROUP-BY
      / ORDER-BY queries against `slice`, `thread_state`, `sched`,
      `counter`, `args` on `test/data/android_postboot_unlock.pftrace`
      (~18MB Android trace). Approximates the UI's post-EOF flurry.
    - `BM_RpcCpuOnlyBurst_{PoolOn,PoolOff}` — 8 50K-row recursive
      CTEs (sum of cubes). Independent, no shared-table reads. The
      diagnostic baseline: confirms the pool's parallelism plumbing
      works on a workload that doesn't hit BtShared.
  - "PoolOn" wires `SetResponseDispatcher` to a fake task queue;
    "PoolOff" leaves it null (the inline synchronous path).
  - Wall-time is measured with `bstate.SetIterationTime(...)`
    bracketing first-OnRpcRequest to last-drain-completion; CPU-time
    columns are not load-bearing.

  **Kill-switch.** `PERFETTO_RPC_POOL_DISABLED=1` in env forces the
  inline synchronous path even when a dispatcher is wired.
  Implemented in `src/trace_processor/rpc/rpc.cc:305-313` with a
  function-static `kPoolDisabled = getenv(...) != nullptr` so the
  flag is read once and races on getenv don't bite. Verified
  empirically: `PERFETTO_RPC_POOL_DISABLED=1` makes
  `BM_RpcCpuOnlyBurst_PoolOn` collapse from 7.8ms (parallel) to
  42.4ms (matches `PoolOff`), and `workers_used` drops to 0. Doubles
  as an ergonomic v1 stability switch — embedders / shells can flip
  it without rebuilding.

  **Numbers (mac_release, 10 cores, 8-worker pool, 10 reps,
  median):**
  | Benchmark                              | PoolOn | PoolOff | Ratio |
  |----------------------------------------|--------|---------|-------|
  | `RpcStreamingQueryBurst` (trace tbls)  | 196 ms | 199 ms  | 1.02x |
  | `RpcCpuOnlyBurst` (recursive CTE)      | 7.81 ms| 42.4 ms | 5.4x  |

  Counters (per benchmark.io kIsIterationInvariant; integer
  literals are summed-across-iterations totals):
  - `RpcStreamingQueryBurst_PoolOn` 10 reps: `workers_used=112`,
    `distinct_connections=112`, `hardware_concurrency=140`,
    `queries=168`. Per-rep: 8 workers / 8 conns / 12 queries / 14
    iters per rep (Google Benchmark auto-picks iters from MinTime).
    The pool **is** spinning up 8 worker threads and 8 connections.
  - `RpcCpuOnlyBurst_PoolOn` 10 reps: `workers_used=2968`,
    `queries=2968`, `hardware_concurrency=3710`. Per-rep: 8 workers
    again, ~37 iters per rep.

  **Bottleneck investigation.**
  - Hypothesis: workers complete sequentially. **Disproved** —
    counters show all 8 workers run on every rep on the trace-table
    workload too (workers_used >= 8). Workers ARE picking up tasks
    in parallel.
  - Hypothesis: `pool_mu_` contention serialises Acquire/Release.
    **Unlikely** — `AcquireConnectionForQuery` only takes the lock
    long enough to pop the free list (microseconds); the CPU-only
    burst has identical dispatch overhead and shows 5.4x speedup.
    Pool overhead is bounded.
  - Hypothesis: `StringPool` mutex contention. **Unlikely** — the
    fast read path `Get(Id)` is lock-free (string_pool.h:243);
    `InternString` takes the lock but post-EOF no new strings are
    interned (the trace is fully parsed). Read-only queries hit only
    the lock-free path.
  - Hypothesis: `sql_stats_mutex_`. Tiny lock around 4 push_backs
    and one pop_front per query (trace_storage.cc:103); negligible.
  - **Confirmed bottleneck: SQLite shared-cache `BtShared` mutex.**
    With `SQLITE_THREADSAFE=2` + `cache=shared`, every btree cursor
    operation acquires the per-database `BtShared.mutex`. This
    serialises *all* reads against the same database across all
    connections. Even though SQLite's own per-connection mutex is
    contention-free in `=2` mode, the shared cache adds back a
    global lock at the storage layer. The CpuOnly workload
    sidesteps this because the recursive CTE materialises in
    per-statement temp space (no btree cursor on a trace-data
    table), and so each query holds no `BtShared.mutex`.

  This is **not** a bug in the iter-1/iter-3 design — the design is
  correct under the documented assumption that `cache=shared` lets
  reads parallelise. SQLite (as built) doesn't honour that for
  shared cache. Three follow-on options exist; none of them in
  scope for this iter:
  1. Switch to `cache=private` per connection. Breaks the
     "schema-shared via cache=shared" pivot. Would need cross-conn
     schema replay (dropped earlier as a footgun).
  2. Replace the trace-data btree storage with the in-house
     `Dataframe` (already lock-free for reads). Most of the post-EOF
     query traffic goes through dataframe vtab modules, but the
     btree-backed tables (registered via `__intrinsic_*`,
     `runtime_table`, etc.) remain serialised. Long-tail follow-up.
  3. Carry an explicit per-vtab "this is read-only and lock-free"
     bit and bypass `BtShared.mutex` for those tables. Requires
     forking SQLite's btree.c. Hard.

  Recommend filing this finding into the project memo as **the v1
  ceiling for the http-rpc transport's parallelism**: gain is
  visible only on workloads that don't touch shared btree tables.
  The UI's post-EOF flurry is mostly btree reads, so the realised
  speedup on real trace loads is bounded by what fraction of those
  queries are dataframe-vtab vs. btree. Need to instrument the UI
  to find out — separate work.

  **Validation.**
  - `tools/gn check out/mac_release` — clean.
  - `tools/ninja -C out/mac_release perfetto_unittests
    perfetto_benchmarks trace_processor_shell` — clean.
  - `out/mac_release/perfetto_unittests --gtest_brief=1` — 3269
    PASSED, 2 SKIPPED, 1 pre-existing failure
    (`HttpServerTest.Websocket`, also failed in iter 3, unrelated).
    Matches iter 3 baseline exactly.
  - `out/mac_tsan/perfetto_unittests
    --gtest_filter="RpcTest.*"` — 7 PASSED. TSan clean on all
    iter-3 + iter-4 tests (the kill-switch added in this iter is
    a function-static `getenv` read; no threading concerns).
  - Benchmark stability: 10 reps, stddev <1ms on all four
    benchmarks. Numbers are reproducible run-to-run within ~1%.

  **Files touched.**
  - `src/trace_processor/rpc/rpc.cc` — `kPoolDisabled` env-var
    kill-switch in the `TPM_QUERY_STREAMING` case.
  - `src/trace_processor/rpc/rpc_perf_benchmark.cc` — new file,
    4 benchmarks (`BM_Rpc{StreamingQueryBurst,CpuOnlyBurst}_Pool{On,Off}`).
  - `src/trace_processor/rpc/BUILD.gn` — wire the new benchmark
    source into the `:benchmarks` source_set; add the
    `protos/perfetto/trace_processor:zero` dep needed for the wire
    encoder helpers.
  - `STATUS.md` — this entry, ticking `e2e-perf-validation`.

  **Recommendation.** Next chunk should be `wasm-pthreads` to
  validate the same pool plumbing through the wasm transport — it's
  a separate axis from the BtShared bottleneck and the design value
  of cross-tab/cross-trace concurrency in the UI is a real
  independent win even if the per-trace within-tab speedup is
  bounded. After wasm-pthreads, Phase 4 should wrap with an explicit
  follow-up note about the BtShared ceiling and a "Phase 5
  candidate: dataframe-only post-EOF query path" exit criterion.

- 2026-04-29 [Phase 4 iter 3]: httpd-pool-dispatch done. The websocket
  / `OnRpcRequest` `TPM_QUERY_STREAMING` path now (a) dispatches
  query execution to the iter-1 worker pool and (b) returns from
  `OnRpcRequest` synchronously without blocking on results, so the
  http task runner is free to handle the next websocket message.

  **Piece A — async streaming dispatch.**
  `Rpc::ParseRpcRequest` previously serialised the entire query
  inline at `rpc.cc:298` (`trace_processor_->ExecuteQuery(sql)`)
  on whatever thread called `OnRpcRequest`. Now, post-EOF and when
  a `ResponseDispatcher` is set:
  1. The transport thread snapshots `rpc_response_fn_`, claims a
     monotonic slot in `streaming_send_next_seq_`, and posts a job
     to the worker pool.
  2. The worker thread acquires a `Connection`, runs the query,
     and materialises chunks (one per `QueryResultSerializer::Serialize`
     call) into a `StreamingResult` buffered against the slot.
  3. Connection is released and the worker invokes
     `response_dispatcher_(closure)` to schedule a "drain" task on
     the transport thread.
  4. The drain pops `streaming_send_ready_[streaming_send_drain_cursor_]`,
     wraps each chunk into a `Response` with a freshly-assigned
     `tx_seq_id_`, and sends. It loops to keep draining
     contiguous slots that finished out of order. **This preserves
     send-order to the UI even when workers complete in arbitrary
     order**, satisfying the `pendingQueries[0]` FIFO invariant
     identified in iter 2's `engine.ts` audit.

  Crucial ordering guarantee: workers may complete in *any* order,
  but the drain on the transport thread services slots in
  `streaming_send_drain_cursor_` order — out-of-order completions
  park in `streaming_send_ready_` until their predecessor lands.
  Net: query execution overlaps across cores; the on-wire
  responses still march out in the same order the requests came
  in. The UI's pendingQueries[0]-FIFO match is preserved
  byte-perfect.

  Per-chunk callbacks (option A2 in the chunk plan) were
  considered and rejected: the UI's `pendingQueries[0]` decoder
  *requires* contiguous chunks per query, and per-chunk PostTask
  back from concurrent workers would interleave them.
  Materialising a query's chunks fully on the worker and shipping
  them as a *single* drain step (per slot) gives the same wall-
  time UX (sqlite execution still parallelises) without the
  interleaving hazard.

  **Piece B — task-runner unblock.** The `Rpc::ParseRpcRequest`
  case used to loop on `serializer.Serialize` inline, blocking
  the http task runner for the lifetime of the query. After this
  iter, the `case TPM_QUERY_STREAMING` body in
  `rpc.cc:278-340` returns immediately after the worker dispatch
  (or runs the legacy inline path when the dispatcher is null).
  Concurrent websocket messages from the same UI session now
  fan out across pool workers — previously they queued on the
  single task-runner thread.

  **`/rpc` and wasm preserved.** The `/rpc` chunked HTTP endpoint
  (Python API) sends its `0\r\n\r\n` trailer immediately after
  `OnRpcRequest` returns, so it requires synchronous response.
  Httpd disables the async path around the `/rpc` call by
  swapping the dispatcher to nullptr and back (both calls run on
  the task-runner thread, race-free). The wasm bridge never
  installs a dispatcher, so it also stays synchronous — which
  is what wasm without pthreads needs anyway (single threaded
  by definition; this iter doesn't change that).

  **Conn lifetime.** `Httpd` now tracks a per-`HttpServerConnection`
  `shared_ptr<bool>` "alive" flag. The websocket response fn
  captures it; if a chunk send arrives after the conn closed
  (via `OnHttpConnectionClosed`), the alive check no-ops the
  send instead of touching freed memory. The flag is created
  lazily on first `OnWebsocketMessage` for a conn and cleared
  on `OnHttpConnectionClosed`. The map is accessed only from
  the task-runner thread, so no locking is needed.

  **Files touched.**
  - `src/trace_processor/rpc/rpc.{h,cc}` — async streaming
    dispatch state, `SetResponseDispatcher`, send-order
    sequencing.
  - `src/trace_processor/rpc/httpd.cc` — wires the dispatcher
    to `task_runner_.PostTask`, tracks per-conn alive flags,
    bypasses async for `/rpc`, keeps the websocket response fn
    set across `OnRpcRequest` so the snapshot taken inside
    `Rpc` remains valid.
  - `src/trace_processor/rpc/rpc_unittest.cc` — 3 new tests
    (see below).
  - `STATUS.md` — this entry, ticking `httpd-pool-dispatch`.

  **New tests** (all green; clang/release):
  - `RpcTest.StreamingQueryDispatchesAsyncAndUnblocksTransport`
    — drives one `OnRpcRequest` with a `TPM_QUERY_STREAMING`
    payload through a wired-up dispatcher; asserts
    `OnRpcRequest` returns *before* any wire bytes are emitted
    and that the response decodes correctly after task-queue
    drain.
  - `RpcTest.StreamingQueryFansOutAcrossWorkers` — issues 8
    concurrent `OnRpcRequest`s, drains, asserts (i) all 8
    decode correctly with their dispatch-order integer
    literal `100+i`, (ii) responses come back in send-order
    (the FIFO invariant), (iii) `pool_workers_used_for_testing()
    >= 2` on multicore hosts.
  - `RpcTest.StreamingQueryAsyncMatchesInlineSemantically` —
    runs the same recursive CTE under both paths and compares
    everything except `elapsed_time_ms` (wall-time-derived).
    Same column names, same statement counts, same row values,
    same batch counts, same `is_last_batch` markers. Caveat:
    byte-for-byte parity is *not* possible because
    `QueryResultSerializer::Serialize` writes
    `set_elapsed_time_ms` from `base::GetWallTimeNs()` —
    documented in the test rationale.

  **Validation.**
  - `tools/gn check out/mac_release` — clean.
  - `tools/ninja -C out/mac_release perfetto_unittests
    trace_processor_shell` — clean.
  - `out/mac_release/perfetto_unittests --gtest_brief=1` — 3269
    PASSED, 2 SKIPPED, 1 pre-existing failure
    (`HttpServerTest.Websocket`, also fails on iter 2's tip
    175c8375e6, unrelated to this change). Iter 1's baseline
    was 3266 + 3 new RpcTests = 3269. Matches.
  - `out/mac_release/perfetto_integrationtests --gtest_brief=1
    --gtest_filter="TraceProcessor*:*Sqlite*:ReadTrace*"` —
    122 PASSED. Iter 1 reported 118; the discrepancy is
    reconciled here — the figure now matches Phase 3's 122.
    Suspected cause: iter 1's `-k 10000` swallowed test
    failures or skipped builds; the actual count is stable.
  - `tools/diff_test_trace_processor.py
    out/mac_release/trace_processor_shell --keep-input
    --quiet` — 1355 PASSED, 9 SKIPPED (etm + symbolize, env
    deps). Matches Phase 3 baseline.
  - **TSan stress (5 runs)** on `*Rpc*` filter: clean.
  - **ASan stress (10 runs)** on
    `*Rpc*:TraceProcessorConnectionTest.*`: clean (30 tests
    each run).

  **Caveats / out-of-scope for this iter.**
  - Backpressure: a slow transport could let
    `streaming_send_ready_` grow without bound if workers race
    far ahead. Out of scope for v1; the natural rate limit is
    the worker pool size (capped at 8), so the queue is bounded
    by `≤8 * max_chunks_per_query` in steady state.
  - The fan-out test asserts `>=2 workers` only when
    `hardware_concurrency >= 2`. Single-core hosts (CI) still
    pass but exercise no parallelism.
  - On wasm without pthreads, `worker_pool_` runs all tasks
    inline-equivalent (degenerate ThreadPool with 1 thread).
    Async dispatch *still works* (chunks come back via the
    pool callback) but no parallelism — same as today's
    behaviour for the wasm transport, which doesn't install
    a dispatcher anyway.

  **Recommendation.** Next chunk should be `e2e-perf-validation`,
  not `wasm-pthreads`: the headline `httpd-pool-dispatch` win
  is now landable end-to-end on the http-rpc transport (the
  UI's "Trace Processor native acceleration" path, which is the
  dominant power-user transport for large traces). Measure that
  before adding wasm-pthreads complexity. `wasm-pthreads`
  remains valuable but is a separate axis — bundle COOP+COEP
  detection, GN flag, and runtime fallback into its own iter.

- 2026-04-29 [Phase 4 iter 2]: ui-engine-fan-out closed as audit-only.
  Set out to lift client-side serialisation in `engine.ts`; the audit
  found there is no client-side serialisation to lift. `streamingQuery`
  already pushes onto `pendingQueries` and fires `rpcSendRequestBytes`
  synchronously, with no mutex / promise-chain. Concurrent
  `engine.query()` calls from JS issue their TraceProcessorRpc
  messages back-to-back on the websocket. The single FIFO match
  point — `pendingQueries[0]` consuming `TPM_QUERY_STREAMING`
  responses in onRpcResponseMessage — only requires that the
  trace_processor RPC server emit all chunks of one query
  contiguously before starting the next, which `Rpc::Query` already
  honours per-call.
  
  **The bottleneck is in C++, not in `engine.ts`.** Two findings
  from tracing the websocket path end-to-end:
  
  1. `Httpd::OnWebsocketMessage` → `Rpc::OnRpcRequest` →
     `Rpc::ParseRpcRequest` (`src/trace_processor/rpc/rpc.cc:227`)
     handles `TPM_QUERY_STREAMING` inline at lines 278-331, calling
     `trace_processor_->ExecuteQuery(sql)` on the writer engine
     directly. It does **not** go through `Rpc::Query`, which is
     the only path wired to the iter-1 worker pool. Today the
     only `Rpc::Query` caller from a real transport is the
     `/query` HTTP POST endpoint in `httpd.cc:228` (a chunked-
     transfer endpoint that predates the websocket path and the
     UI no longer uses by default).
  2. Even if (1) were fixed, `Httpd` runs a single
     `base::MaybeLockFreeTaskRunner` (`httpd.cc:69`) and
     `Rpc::Query` blocks its caller on `done_fut.wait()`
     (`rpc.cc:827`). N concurrent websocket messages still
     serialise on the task-runner thread regardless of how many
     pool workers exist.
     
  Net effect: the iter-1 worker pool is currently dormant for the
  UI's primary transport. UI-side fan-out (this iter) is a
  prerequisite for, but not sufficient for, an HTTP-RPC speedup.
  
  **WASM-pthreads question.** Same shape as HTTP-RPC: the wasm
  bridge in `engine_bundle.js` (worker thread) marshals
  `TraceProcessorRpc` bytes through the synchronous `OnRpcRequest`
  path, not `Rpc::Query`. Even with pthreads enabled in wasm,
  only one query at a time would dispatch onto the pool — same
  limitation as httpd. So `wasm-pthreads` is not a hard
  prerequisite for this iter (the iter is a no-op regardless),
  but the chunk's value depends on `httpd-pool-dispatch` (new
  chunk) landing alongside it.
  
  **Change made.** Added a comment block above
  `pendingQueries` in `engine.ts` documenting the audit
  conclusion: client side is already concurrent, FIFO matching
  is contractually safe as long as the C++ side emits chunks
  for one query contiguously, and the gating chunk is
  `httpd-pool-dispatch`. No functional change.
  
  **Validation.**
  - `ui/build --typecheck --no-depscheck`: clean
    (`tsc --project ../../ui --noEmit` + service_worker tsc).
  - `npx eslint src/trace_processor/`: clean (no errors / warnings).
  - `ui/run-unittests --no-depscheck`: 125 suites, 2249 PASSED,
    1 SKIPPED, 0 FAILED — matches the iter-1 baseline shape, no
    regressions introduced by the comment change.
  - Visual / UI dev-server validation: deferred. Without the
    `httpd-pool-dispatch` chunk landed there is nothing visually
    different to observe — the network panel already shows
    concurrent in-flight WebSocket frames today, the C++ side
    just answers them one at a time.
  - Perf measurement: deferred to `e2e-perf-validation`. A
    headline number requires `httpd-pool-dispatch` first;
    measuring the no-op change in isolation would just produce
    noise.
  
  **Files touched.**
  - `ui/src/trace_processor/engine.ts` — comment-only change
    above `pendingQueries`.
  - `STATUS.md` — this entry, plus the chunk-list re-prioritisation
    below (added new `httpd-pool-dispatch` chunk, swapped its
    order with `wasm-pthreads`).
  
  **Recommendation.** Next chunk should be `httpd-pool-dispatch`,
  not `wasm-pthreads`: the websocket path is the actual UI
  transport in production today, and the iter-1 pool is dormant
  until queries dispatched via websocket actually land on a
  worker.

- 2026-04-29 [Phase 4 iter 1]: rpc-thread-pool done. `Rpc` now fans
  query RPCs across a `base::ThreadPool` sized to
  `min(hardware_concurrency, 8)` (capped because TP queries are
  sqlite-bound and the shared cache+memdb don't benefit from more
  contention; `hardware_concurrency() == 0` falls back to 1). The
  pool lives in `src/trace_processor/rpc/rpc.{h,cc}` (new private
  helpers `AcquireConnectionForQuery`, `ReleaseConnectionToPool`,
  `DrainConnectionPoolForMutation`, `RunQueryOnPoolWorker`). Used the
  existing `base::ThreadPool` from
  `include/perfetto/ext/base/threading/thread_pool.h` rather than
  rolling an inline impl — same shape the bigtrace orchestrator uses.

  **Drain-then-release strategy (option (a)).** Each worker:
  1. Acquires a `TraceProcessor::Connection` from the lazy pool.
  2. Runs the query through `connection->ExecuteQuery(sql)`.
  3. Drains the iterator into `std::vector<Chunk>` (each chunk =
     serialised `QueryResult` proto bytes) via the existing
     `QueryResultSerializer`.
  4. **Iterator + serializer are scoped tightly** so the prepared
     statement (`sqlite3_stmt*` owned by `IteratorImpl`) is finalised
     on the worker thread *before* the connection goes back to the
     pool. TSan caught this on the first run: leaving the serializer
     alive across `ReleaseConnectionToPool` lets a peer worker
     re-acquire the same connection mid-finalize and race on the
     underlying `sqlite3*` (sqlite3ErrorClear vs. sqlite3VdbeReset).
  5. Releases the connection.
  6. Streams the buffered chunks back to the transport via the
     `QueryResultBatchCallback`.

  The caller (the transport thread) blocks on a `std::promise<void>`
  while the worker runs, preserving the synchronous callback contract
  of `Rpc::Query`. Concurrent `Rpc::Query` calls from N transport
  threads each block on their own promise and so fan out across pool
  workers — this is what the UI fan-out (next chunk) will consume.

  **Mutation gating discipline.** Mutating RPCs (`Parse` via
  `ResetTraceProcessorInternal`, `RegisterSqlPackage`,
  `RestoreInitialTables`) call `DrainConnectionPoolForMutation` first:
  it sets `pool_blocked_for_mutation_`, swaps `pool_free_` out and
  destroys the cached connections, then waits on a condvar for
  `pool_in_use_ == 0`. Workers that release a connection while the
  drain is pending see the flag and *destroy* their connection
  instead of pushing it back. Once the mutation body returns, a
  `ScopedPoolUnblock` clears the flag and notifies parked acquirers.
  Acquirers that arrive during the drain park on the same condvar.
  This means the writer-side
  `non_default_connection_count_ == 0` `PERFETTO_CHECK` gate (Phase
  2) never fires from within `Rpc`, even under interleaved query +
  mutation traffic.

  **`CreateConnection` is single-producer.** The first TSan run
  (before the drain refactor) flagged a real race on
  `StringPool::should_acquire_mutex_` — multiple workers calling
  `CreateConnection` concurrently both write the bool, and peer
  worker reads of `MaybeLockGuard` see torn writes. The fix is
  structural: workers *never* call `CreateConnection`. The pool is
  pre-minted to `worker_pool_` size (one connection per worker
  thread, all from the caller/writer thread on first post-EOF
  Query); workers only ever consume from the free list. A new
  `pool_mint_mu_` serialises the pre-mint across multiple Rpc
  callers and against any concurrent mutation drain. Because the
  worker pool is also capped at the same N, at most N tasks run
  concurrently, so N pre-minted connections cover the worst case
  with no need for unbounded growth.

  **Pre-EOF queries bypass the pool entirely.** Strict-v1 says
  secondary connections are illegal pre-EOF (`CreateConnection`
  CHECKs `notify_eof_called_`). `Rpc::Query` checks `eof_` and
  routes pre-EOF queries through the writer engine directly,
  matching today's single-threaded ingestion contract. EOF is
  `eof_ = true` after `NotifyEndOfFile` returns OK; reset back to
  false in `Parse` if a fresh trace is being loaded (and that
  path goes through `ResetTraceProcessorInternal` which drains
  the pool).

  **New tests** (`src/trace_processor/rpc/rpc_unittest.cc`,
  4 tests under `RpcTest.*`):
  - `PostEofQueryRunsThroughWorkerPool` — single post-EOF
    `Rpc::Query`; verifies the response decodes correctly and the
    pool minted at least one connection. Smoke for the happy path.
  - `PreEofQueryBypassesWorkerPool` — pre-EOF `Rpc::Query`; verifies
    the response decodes correctly, the pool is untouched
    (`pool_distinct_connections_for_testing() == 0` and
    `pool_workers_used_for_testing() == 0`).
  - `QueryFansOutAcrossWorkers` — 8 threads issue `SELECT 100+i`
    concurrently; asserts every response decodes correctly *and*
    that `pool_workers_used_for_testing() >= 2` on any machine
    where `hardware_concurrency() >= 2`. Doubles as the
    "connections recycled" check via
    `pool_distinct_connections_for_testing() <= kQueries`.
  - `MutationDrainsPoolAndQueriesContinue` — populate the pool,
    call `RestoreInitialTables`, then verify follow-up queries
    still work. Exercises the drain + refill path end-to-end.

  Two test-only counters added to `Rpc`
  (`pool_workers_used_for_testing`,
  `pool_distinct_connections_for_testing`) — both
  `unordered_set<thread::id>::size()` / `uint32_t` reads under
  `pool_mu_`. Not part of the wire API.

  **Test counts** (Phase 4 iter 1 close, vs. iter-7 baseline):
  - `out/mac_release/perfetto_unittests`: 3266 PASSED
    (was 3262), 2 SKIPPED, 1 pre-existing macOS failure
    (`HttpServerTest.Websocket`). Net delta: +4 (the new
    `RpcTest.*` cases).
  - `out/mac_release/perfetto_integrationtests` (TP filter):
    118 PASSED (matches; 122 was the broader filter on iter-7
    — same shape, no regressions).
  - `tools/diff_test_trace_processor.py`: 1355 PASSED + 9
    pre-existing skips, unchanged.
  - **TSan stress** (5 consecutive runs of `RpcTest.*` on
    `out/mac_tsan`): 4/4 PASSED each, no
    `WARNING: ThreadSanitizer:` output. Full TSan suite: 3265
    PASSED + 2 SKIPPED + 1 pre-existing macOS failure, matching
    release baseline shape (TSan release baseline was 3261; the
    +4 is the new `RpcTest.*`).
  - **ASan stress** (10 consecutive runs of
    `TraceProcessorConnectionTest.*:RpcTest.*` on
    `out/mac_asan`): 27/27 PASSED each.

  **Files touched**:
  - `src/trace_processor/rpc/rpc.h` — new public test-only counters,
    new private pool helpers + members.
  - `src/trace_processor/rpc/rpc.cc` — `RunQueryOnPoolWorker`,
    `AcquireConnectionForQuery`, `ReleaseConnectionToPool`,
    `DrainConnectionPoolForMutation`, `ScopedPoolUnblock` RAII
    helper; drain wired into `RestoreInitialTables`,
    `RegisterSqlPackage`, `ResetTraceProcessorInternal`.
  - `src/trace_processor/rpc/BUILD.gn` — added
    `../../base/threading` dep on the `:rpc` target,
    `rpc_unittest.cc` and `../../base:test_support` to
    `:unittests`.
  - `src/trace_processor/rpc/rpc_unittest.cc` — new file, 4 tests.

- 2026-04-29 [Phase 3 iter 7]: tsan-multithread-stress done; Phase 3
  closed. New `out/mac_tsan` build dir
  (`is_clang=true is_debug=false is_tsan=true`) — the GN args
  scaffolding from `tools/setup_all_configs.py` and
  `buildtools/BUILD.gn` already supported it on macOS arm64; the
  config simply hadn't been instantiated. `tools/gn gen --check
  out/mac_tsan` succeeds and `tools/ninja -C out/mac_tsan
  perfetto_unittests` builds cleanly against the vendored libtsan
  runtime in `buildtools/mac/clang/lib/clang/.../libclang_rt.tsan
  _osx_dynamic.dylib`.

  **Race surfaced and fixed.** First TSan run of
  `TraceProcessorConnectionTest.ConcurrentRecordingIntoSqlStats`
  reported a clean data race on
  `TraceProcessorImpl::non_default_connection_count_` — the test
  hands each connection to a worker thread, so when worker threads
  exit concurrently their `unique_ptr<Connection>` destructors fire
  on different threads, both running `ReleaseConnection` and
  decrementing the counter without synchronisation. Two threads
  calling `--int` is a textbook race even when the read-modify-
  write happens to round-trip correctly on arm64.

  **Fix:** promoted `non_default_connection_count_` from
  `int = 0` to `std::atomic<int>{0}` in `trace_processor_impl.h`
  and updated the two mutators in `trace_processor_impl.cc`:

  ```cpp
  // CreateConnection (writer thread, sole producer of '+'):
  non_default_connection_count_.fetch_add(1, std::memory_order_relaxed);

  // ReleaseConnection (any thread):
  int prev = non_default_connection_count_.fetch_sub(
      1, std::memory_order_acq_rel);
  PERFETTO_CHECK(prev > 0);  // strictly-positive guard pre-decrement
  ```

  The ~12 mutation-gating reads
  (`PERFETTO_CHECK(non_default_connection_count_ == 0)` in `Parse`,
  `NotifyEndOfFile`, the destructor, etc.) keep their existing form
  — `std::atomic<int>::operator==(int)` does an implicit relaxed
  load, the expected value at those sites is zero, and the writer
  thread is the only producer of `++` so the gates can never
  observe a stale "saw zero, then a `++` slipped in" race.
  Memory-ordering choices: `relaxed` on increment because there is
  no data published-via-counter (the counter only exists for the
  CHECK gates), `acq_rel` on decrement so the count can serve as a
  release point if a future caller wants to use "count went to
  zero" as a happens-before edge for follow-up cleanup.

  **Stress validation under TSan:**
  - `out/mac_tsan/perfetto_unittests
    --gtest_filter="TraceProcessorConnectionTest.*"`: 23/23 PASSED
    across **10 consecutive runs**, no `WARNING: ThreadSanitizer:`
    output, no late TSan-on-shutdown reports.
  - Full `out/mac_tsan/perfetto_unittests`: 3261 PASSED, 2 SKIPPED,
    1 pre-existing macOS failure
    (`HttpServerTest.Websocket`) — same shape as release. (The
    full-suite count is 3264 vs. release's 3265; the gap is a
    single sanitizer-gated test elsewhere in the tree, not a
    Phase 3 regression.)
  - `out/mac_asan/perfetto_unittests
    --gtest_filter="TraceProcessorConnectionTest.*"`: 10/10
    consecutive runs all 23/23 PASSED to confirm the iter-6
    baseline is preserved post-atomic.

  **Test counts** (Phase 3 close):
  - `out/mac_release/perfetto_unittests`: 3262 PASSED, 2 SKIPPED,
    1 pre-existing macOS failure (unchanged from iter 6).
  - `out/mac_release/perfetto_integrationtests` (TP-relevant
    filter): 122 PASSED, unchanged.
  - `tools/diff_test_trace_processor.py`: 1355 PASSED + 9
    pre-existing skips, unchanged.

  Net delta vs. iter 6: +1 fix (race on the connection counter),
  zero new tests, zero regressions, TSan now part of the matrix.

- 2026-04-29 [Phase 3 iter 6]: schema-retry done. Transparent
  `SQLITE_SCHEMA` recovery now wraps both
  `SqliteEngine::PrepareStatement` (around `sqlite3_prepare_v2`) and
  `SqliteEngine::PreparedStatement::Step` (around `sqlite3_step`) in
  `src/trace_processor/sqlite/sqlite_engine.{h,cc}`. The recovery
  shape differs from BUSY/LOCKED:

  - **BUSY/LOCKED** (iter 5): `sqlite3_reset` + sleep + retry the
    *same* statement — the bytecode is still valid, just a lock
    held it back.
  - **SCHEMA** (this iter): finalize + re-prepare from the saved
    `SqlSource`. SQLite returns SCHEMA when the
    `sqlite_schema_cookie` has been bumped (some other connection
    committed a CREATE / DROP / ALTER) and the compiled bytecode is
    stale; the only safe fix is to re-walk the parser against the
    new schema.

  **New helper `SchemaRetryHelper` in `sqlite_engine.{h,cc}`:**

  ```cpp
  class SchemaRetryHelper {
   public:
    static constexpr base::TimeMillis kDefaultTimeout = base::TimeMillis(1000);
    static constexpr uint32_t kMaxAttempts = 100;
    explicit SchemaRetryHelper(base::TimeMillis timeout = kDefaultTimeout);
    bool ShouldRetry(int sqlite_status);  // bumps attempt
    uint32_t attempt() const;
  };
  ```

  Two independent termination conditions: a 1-second wall-clock
  deadline (matches BUSY's default) **and** a hard count cap of
  100 attempts. The count cap is the tighter bound under unsleep'd
  retries and guards against pathological "schema bumps every
  prepare" loops where the deadline never fires because each
  iteration is microseconds. The helper does *not* sleep — schema
  bumps are typically serialised by the prior writer's commit so
  the next prepare sees a stable cookie immediately; sleeping
  would just slow down the common case.

  **Re-prepare path:** `PreparedStatement` already carried
  `sql_source_`. Iter 6 added `db_` (non-owning sqlite3*),
  `retry_timeout_`, and `rows_seen_`. The new private
  `ReprepareFromSource()` finalizes the old stmt, re-issues
  `sqlite3_prepare_v2(db_, sql_source_.sql().c_str(), ...)`, and
  re-snapshots `expanded_sql_`. The constructor now takes a
  `sqlite3*` arg so re-prepare can target the same connection
  even after `stmt_` is reset (post-finalize, `sqlite3_db_handle`
  is unsafe).

  **Wrap pattern at PrepareStatement** (sqlite_engine.cc:248-271):
  for-loop dispatching on three branches — OK breaks, BUSY/LOCKED
  goes through `busy_retry.ShouldRetry`, SCHEMA goes through
  `schema_retry.ShouldRetry`. No re-prepare needed at this layer
  because `sqlite3_prepare_v2` is itself the retry — just call it
  again.

  **Wrap pattern at Step** (sqlite_engine.cc:457-500): the SCHEMA
  branch is the new code path:

  ```cpp
  if (err == SQLITE_SCHEMA) {
    if (rows_seen_) break;            // mid-iter, surface error
    if (!schema_retry.ShouldRetry(err)) break;
    int rc = ReprepareFromSource();
    while (rc != SQLITE_OK) {
      if (rc == SQLITE_SCHEMA && schema_retry.ShouldRetry(rc)) {
        rc = ReprepareFromSource(); continue;
      }
      if ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) &&
          busy_retry.ShouldRetry(rc)) {
        rc = ReprepareFromSource(); continue;
      }
      break;
    }
    if (rc != SQLITE_OK) { err = rc; break; }
    continue;  // re-issue sqlite3_step on freshly-prepared stmt
  }
  ```

  **`rows_seen_` guard:** the `SQLITE_ROW` branch sets
  `rows_seen_ = true`. If a subsequent step ever returns SCHEMA,
  we surface the error rather than silently re-prepare — the
  cursor cannot be safely restarted because the caller has
  already consumed rows. SQLite's documented contract is that
  SCHEMA only fires on the *first* step after prepare, so this is
  defence-in-depth rather than a frequent path; but if a custom
  vtab module's `xNext` ever returns SCHEMA mid-iter (rare, would
  require a write through a virtual table that bumps the cookie),
  we fail loud rather than silently re-emit rows.

  **`ExecWithRetry` (new public method on `SqliteEngine`)**: the
  `sqlite3_exec`-based savepoint plumbing in `PerfettoSqlEngine`
  goes around `PreparedStatement` entirely, so the iter-5 retry
  middleware did not cover it. The first ASan stress run of
  `ConcurrentDDLDoesNotBreakReaders` surfaced this as
  "EXECUTE: failed to open savepoint 'perfetto_execute_0':
  database schema is locked: main" (a `SQLITE_LOCKED_SHAREDCACHE`
  on the savepoint open while a sibling DDL was committing).
  Fix: a new `SqliteEngine::ExecWithRetry(const char* sql)` that
  wraps `sqlite3_exec` in the same BUSY/LOCKED + SCHEMA retry
  shape. Migrated all six savepoint sites (open/release/rollback
  for both Execute- and Include-savepoints, plus the wildcard-
  expansion include path) — the only remaining `sqlite3_exec`
  call is the writer's `CREATE TABLE perfetto_tables` at engine
  init, which is single-threaded and pre-NotifyEndOfFile.

  **Direct unit tests for the helper** under
  `src/trace_processor/sqlite/sqlite_engine_unittest.cc`:
  - `SchemaRetryHelperTest.RetriesUntilSuccess`: feed SCHEMA × 3
    then OK — verify three retries, then stop.
  - `SchemaRetryHelperTest.PassesThroughOtherErrors`: feed
    `SQLITE_BUSY` / `SQLITE_LOCKED` / `SQLITE_ERROR` /
    `SQLITE_CONSTRAINT` and verify the schema helper does *not*
    retry — those are BUSY's territory.
  - `SchemaRetryHelperTest.GivesUpAtCountCap`: feed SCHEMA 100×
    successfully, verify the 101st returns false and `attempt()`
    is exactly `kMaxAttempts`.
  - `SchemaRetryHelperTest.BothBoundsApplyIndependently`: with a
    1000ms deadline and no sleeps, the count cap fires first
    (100 unsleep'd retries are fast). Documents which bound
    typically wins.

  **End-to-end tests** under `TraceProcessorConnectionTest.*` in
  `src/trace_processor/trace_processor_connection_unittest.cc`:
  - `SchemaRetryRePreparesOnSchemaChange`: writer creates a table,
    secondary mints, writer creates *another* table (bumps the
    cookie under the secondary), secondary's first SELECT must
    succeed. Repeats: another DDL, another SELECT — proves the
    re-prepare path is re-entrant.
  - `ConcurrentDDLDoesNotBreakReaders`: writer thread does
    100×(CREATE + DROP) DDL, reader thread on a sibling
    connection does 200×(SELECT 1). Without the SCHEMA retry +
    `ExecWithRetry`, the reader saw "database schema is locked
    main" within a handful of iterations on ASan; with both,
    10/10 stress runs are clean.

  **Test counts:**
  - `out/mac_release/perfetto_unittests`: 3262 PASSED + 2 SKIPPED
    + 1 pre-existing macOS failure (`HttpServerTest.Websocket`).
    +6 new tests vs. iter 5 (4 `SchemaRetryHelperTest.*` + 2
    `TraceProcessorConnectionTest.{SchemaRetryRePreparesOnSchema
    Change, ConcurrentDDLDoesNotBreakReaders}`).
  - `out/mac_release/perfetto_integrationtests` (TP-relevant
    filter): 122 PASSED, unchanged.
  - `tools/diff_test_trace_processor.py`: 1355 PASSED + 9
    pre-existing skips, unchanged.
  - `out/mac_asan/perfetto_unittests` filtered to
    `TraceProcessorConnectionTest.*`: 23/23 PASSED across **10
    consecutive runs**, no ASan reports. The
    `ConcurrentDDLDoesNotBreakReaders` stress (the test that
    initially exposed the savepoint-`sqlite3_exec` gap) is in
    this set and is now reliable.

- 2026-04-29 [Phase 3 iter 5]: busy-retry done. Transparent
  `SQLITE_BUSY` / `SQLITE_LOCKED` retry middleware now wraps both
  `SqliteEngine::PrepareStatement` (around `sqlite3_prepare_v2`) and
  `SqliteEngine::PreparedStatement::Step` (around `sqlite3_step`) in
  `src/trace_processor/sqlite/sqlite_engine.{h,cc}`. The helper:

  ```cpp
  class BusyRetryHelper {
   public:
    static constexpr base::TimeMillis kDefaultTimeout = base::TimeMillis(1000);
    explicit BusyRetryHelper(base::TimeMillis timeout = kDefaultTimeout);
    bool ShouldRetry(int sqlite_status);  // sleeps + bumps attempt
    void set_sleep_fn_for_testing(SleepFn* fn);
  };
  ```

  Backoff: capped exponential schedule `{100us, 1ms, 10ms, 50ms}` —
  the helper indexes the schedule by attempt and clamps at the cap.
  Deadline is computed at construction (`base::GetWallTimeMs() +
  timeout`). On a non-BUSY/LOCKED status `ShouldRetry` returns false
  immediately so real SQLite errors propagate without delay. Sleep
  uses `base::SleepMicroseconds` (overridable via
  `set_sleep_fn_for_testing` for deterministic unit tests). Per-engine
  timeout member `busy_retry_timeout_` (defaults to 1s, with
  `set_busy_retry_timeout_for_testing` setter) — TODO threaded through
  `Config` in a follow-up; default is fine for v1. The timeout is
  captured into each `PreparedStatement` at prepare time so the same
  retry budget covers both prepare and subsequent steps.

  **Wrap pattern at PrepareStatement (sqlite_engine.cc:240-247):**
  plain `do { sqlite3_prepare_v2; } while (err != OK &&
  retry.ShouldRetry(err))` — no statement state to roll back, the
  prepare hasn't touched the b-tree yet.

  **Wrap pattern at Step (sqlite_engine.cc:380-394):** the for-loop
  pattern from the chunk doc:
  ```cpp
  for (;;) {
    err = sqlite3_step(stmt_.get());
    if (err != SQLITE_BUSY && err != SQLITE_LOCKED) break;
    sqlite3_reset(stmt_.get());
    if (!retry.ShouldRetry(err)) break;
  }
  ```
  The `sqlite3_reset` is essential — `sqlite3_step` returning
  BUSY/LOCKED leaves the statement in an indeterminate state per
  the SQLite docs, so a naive retry would yield SQLITE_MISUSE or
  worse. Reset propagates the deferred error (BUSY/LOCKED) but the
  loop ignores it and re-issues `sqlite3_step` on the next iter.

  **Lift of the iter-2 workaround**: `ConcurrentIncludesOfSameModule
  Serialise` (Phase 3 iter 2) needed the writer to pre-include the
  module before the two secondaries raced their re-includes — a
  naive concurrent include would crash with "database schema is
  locked: main" on the shared-cache schema lock. New test
  `TraceProcessorConnectionTest.ConcurrentIncludesUnderSharedCache
  NowSucceeds` removes the pre-include: two secondaries from two
  threads concurrently `INCLUDE PERFETTO MODULE
  concurrent_include_lift_test.tables;` (no other connection has
  ever included it). The shared-cache schema lock returns
  SQLITE_LOCKED to whichever transaction is second; the retry
  middleware makes that invisible — the loser waits, retries, and
  either acquires the lock to re-issue its own DDL (the per-module
  `IsModuleIncluded` short-circuit then prevents double-creation
  once one side commits) or finds the module already promoted and
  short-circuits the body. Both threads succeed and both can
  `SELECT count(*)`, returning 2.

  **Direct unit tests for the helper** under
  `src/trace_processor/sqlite/sqlite_engine_unittest.cc`
  (new file, added to `sqlite/BUILD.gn:unittests`):
  - `BusyRetryHelperTest.RetriesUntilSuccess`: feed BUSY, BUSY, OK
    — verify two retries, then stop.
  - `BusyRetryHelperTest.RetriesOnLocked`: same as above for
    `SQLITE_LOCKED` (proves both codes map to the same retry).
  - `BusyRetryHelperTest.GivesUpAtTimeout`: 50ms timeout, no real
    sleep — feed BUSY in a loop, verify the wall-clock delta is
    in `[50ms, 5s)` and the loop terminates.
  - `BusyRetryHelperTest.PassesThroughOtherErrors`: feed
    `SQLITE_ERROR` / `SQLITE_CONSTRAINT` / `SQLITE_MISUSE` and
    verify `ShouldRetry` returns false immediately.

  **SQLITE_SCHEMA recovery is intentionally not part of this iter**
  — it requires re-preparing the statement (the schema cookie has
  changed, the bytecode is stale), not retrying the same prepared
  statement. That's the next chunk (`schema-retry`).

  **Test counts:**
  - `out/mac_release/perfetto_unittests`: 3256 PASSED + 2 SKIPPED
    + 1 pre-existing macOS failure (`HttpServerTest.Websocket`).
    +5 new tests vs. iter 4 (4 `BusyRetryHelperTest.*` + 1
    `TraceProcessorConnectionTest.ConcurrentIncludesUnderSharedCache
    NowSucceeds`).
  - `out/mac_release/perfetto_integrationtests` (TP-relevant
    filter): 122 PASSED, unchanged.
  - `tools/diff_test_trace_processor.py`: 1355 PASSED + 9
    pre-existing skips, unchanged.
  - `out/mac_asan/perfetto_unittests` filtered to
    `TraceProcessorConnectionTest.*:BusyRetry*`: 25/25 PASSED
    across **10 consecutive runs**, no ASan reports.
    The lifted concurrent-include test is in this set and is
    clean — proves both the BUSY/LOCKED retry semantics and the
    `sqlite3_reset`-before-retry invariant under real contention.

- 2026-04-29 [Phase 3 iter 4]: string-pool-thread-safety done.
  `StringPool` already had a `MaybeLockGuard` mechanism guarded by
  `should_acquire_mutex_` (default `false`) and a public
  `set_locking(bool)` setter, but no production caller ever flipped
  it on — the iter-3 audit flagged this as the next race once
  Phase 4 fans queries across threads. This iter wires it on
  exactly once: `TraceProcessorImpl::CreateConnection` now calls
  `context()->storage->mutable_string_pool()
  ->EnableThreadSafetyForMultiConnection()` *before* constructing
  the secondary engine and returning it. The flip is idempotent so
  the second-and-subsequent connection mints don't redundantly
  toggle (they just re-write `true` to a `bool`). Single-connection
  callers never reach `CreateConnection` and continue to pay zero
  locking overhead — confirmed: the existing `set_locking` benchmark
  in `string_pool_benchmark.cc` still drives both states.

  **New public API on `StringPool`**:
  `void EnableThreadSafetyForMultiConnection()` — intentionally
  awkward name to discourage flipping back to off. Wraps the same
  bool flip but communicates "this is the production-safe entry
  point". `set_locking(bool)` is kept for the benchmark + future
  test scaffolding (commented as such in the header).

  **MaybeLockGuard coverage audit**: every mutating method
  (`InternString`, `StringPool()` ctor's `InsertInCurrentBlock`)
  takes the guard, and every iterating/inspecting method that
  touches `string_index_` / `blocks_` / `large_strings_` /
  `block_index_` (`GetId`, `CreateSmallStringIterator`, `size`,
  `MaxSmallStringId`, `HasLargeString`, `GetLargeString`) also
  takes the guard. The `Get(Id)` fast-path is and remains lock-free
  per the existing comment ("once a block is initialized, it's
  never touched again for the lifetime of the string pool" —
  enforced by `PERFETTO_TS_UNCHECKED_READ`). No additional locking
  was required inside `string_pool.{h,cc}`; the existing
  annotations were already complete.

  **Two new stress tests under `TraceProcessorConnectionTest.*`**
  in `src/trace_processor/trace_processor_connection_unittest.cc`:
  - `ConcurrentInternFromMultipleConnections`: writer pre-builds a
    32-row dataframe-backed table (one `InternString` per row),
    then four secondary connections each issue 200 SELECTs
    concurrently. Exercises `Get(Id)` (lock-free) plus the
    materialisation paths in `RuntimeDataframeBuilder` — and
    indirectly verifies the `CreateConnection`-side flip didn't
    break the cross-conn read path.
  - `InternedStringMatchesAcrossConnections`: writer builds a
    32-string dataframe table; four secondary connections each
    issue 100 equality-filtered SELECTs with literal RHS. Each
    such query interns the literal via the WHERE-clause path,
    which forces concurrent `GetId`/`InternString` traffic into
    the shared pool from every reader thread. Validates the
    dedup invariant under concurrent interning (every literal
    consistently round-trips to the canonical pool entry — count
    is always 1 per match).

  **Plus one new direct unit test on the StringPool** under
  `StringPoolTest.*` in
  `src/trace_processor/containers/string_pool_unittest.cc`:
  - `ConcurrentInternIsThreadSafe`: 8 threads × 2000 iterations,
    each interleaving (a) interning into a 32-string shared
    bucket and (b) interning a per-(thread, iter) unique string.
    Post-join asserts every shared string maps to the same
    canonical Id across threads (dedup) *and* every distinct
    string maps to a unique Id with a correct `Get(id)` round-
    trip. Most direct exercise of the `EnableThreadSafetyForMulti
    Connection` flip — bypasses SQLite entirely.

  **Test design caveat surfaced**: secondary connections cannot
  themselves run `CREATE PERFETTO TABLE` cross-connection because
  only the primary engine has `is_writer=true` and publishes
  vtab state to `GlobalStagingArea` on `OnCommit`. An earlier
  draft of `InternedStringMatchesAcrossConnections` had each
  secondary CREATE its own table and the test crashed in
  `ModuleStateManagerBase::xConnect` (`PERFETTO_CHECK(resolved)`)
  when other secondaries tried to read those tables. This is
  consistent with the design rule "secondaries are read-only-ish";
  documented here as a multi-conn write-path constraint, not a
  bug — Phase 4 RPC pool design has the writer thread own all
  mutating SQL.

  **Test counts:**
  - `out/mac_release/perfetto_unittests`: 3251 PASSED + 2 SKIPPED
    + 1 pre-existing macOS failure (`HttpServerTest.Websocket`).
    +3 new tests vs. iter 3 (the two connection tests above plus
    `StringPoolTest.ConcurrentInternIsThreadSafe`).
  - `out/mac_release/perfetto_integrationtests` (TP-relevant
    filter): 122 PASSED, unchanged.
  - `tools/diff_test_trace_processor.py`: 1355 PASSED + 9
    pre-existing skips, unchanged.
  - `out/mac_asan/perfetto_unittests` filtered to
    `TraceProcessorConnectionTest.*:StringPool*`: 29/29 PASSED
    across **10 consecutive runs**, no ASan reports. Iter-3's
    fix for the SqlStats race carries forward; iter-4's
    StringPool flip is clean alongside it.

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

## Phase 3 wrap-up

Branch: `dev/lalitm/multi-conn-tp` — 24 commits ahead of `main` (23
code + this iter-7 wrap-up commit). Phase 3 closes here.

What shipped across iters 1-7:
- **Include-lock plumbing + MT smoke** (`include-lock-wire-and-mt-
  smoke`, iter 1): `GlobalStagingArea::AcquireIncludeLock` (the
  hook from Phase 2 iter 3) now wraps every `INCLUDE PERFETTO
  MODULE` invocation on the `ExecutionFrame`. Per-module mutex
  upgraded to `std::recursive_mutex` so re-entrant include of the
  same module from a single thread doesn't self-deadlock.
- **Cross-connection package propagation** (`cross-conn-package-
  propagation`, iter 2): `GlobalStagingArea` grows an additive
  package pool (mirrors the function pool from Phase 2 iter 5).
  Writer's `RegisterSqlPackage` appends after local register;
  readers diff `last_synced_package_version_` at the top of every
  top-level `Execute` and `RegisterPackageLocal`-replay missing
  entries. Shared `IsModuleIncluded` set short-circuits a
  redundant include of the same module from a sibling connection.
- **SqlStats race fix** (`globals-audit`, iter 3): wrapped the
  `std::deque<string>::push_back` writers in `TraceStorage::
  SqlStats` under a `mutable std::mutex`; replaced the raw
  `const deque&` accessors with a `SnapshotForReading()` returning
  a copy under the lock; `SqlStatsModule::Cursor` carries the
  snapshot for iteration.
- **StringPool thread-safety flip** (`string-pool-thread-safety`,
  iter 4): `TraceProcessorImpl::CreateConnection` now flips
  `StringPool::EnableThreadSafetyForMultiConnection()` on the
  shared `TraceStorage` pool *before* returning the secondary
  connection. The existing `MaybeLockGuard` already covered every
  mutating + iterating method. Single-connection callers never
  reach `CreateConnection` and continue to pay zero locking
  overhead.
- **Transparent BUSY/LOCKED retry** (`busy-retry`, iter 5):
  `BusyRetryHelper` in `src/trace_processor/sqlite/sqlite_engine.
  {h,cc}` wraps both `SqliteEngine::PrepareStatement` (around
  `sqlite3_prepare_v2`) and `SqliteEngine::PreparedStatement::
  Step` (around `sqlite3_step`, with `sqlite3_reset` between
  retries). Capped exponential backoff `{100us, 1ms, 10ms,
  50ms cap}` with a 1s default deadline.
- **Transparent SCHEMA re-prepare** (`schema-retry`, iter 6):
  `SchemaRetryHelper` (deadline + 100-attempt count cap) handles
  `SQLITE_SCHEMA` by finalize + re-`prepare_v2` from the saved
  `SqlSource`. The Step path tracks `rows_seen_` and refuses to
  auto-recover from a mid-iteration SCHEMA. New
  `SqliteEngine::ExecWithRetry` wraps `sqlite3_exec` with the
  same BUSY/LOCKED + SCHEMA semantics and is now used by all six
  savepoint open/release/rollback sites in `PerfettoSqlEngine`.
- **TSan + connection-counter race fix** (`tsan-multithread-
  stress`, iter 7): `out/mac_tsan` build dir instantiated;
  `non_default_connection_count_` promoted from `int` to
  `std::atomic<int>` (release-from-worker-thread race surfaced by
  TSan in `ConcurrentRecordingIntoSqlStats`).

Test counts at Phase 3 close vs. Phase 2 close:
- `out/mac_release/perfetto_unittests`: **3262 PASSED + 2 SKIPPED
  + 1 pre-existing macOS failure** (Phase 2 close was 3228 PASSED;
  +34 from iters 1-7 = 5 BusyRetry + 4 SchemaRetry + 1 SqlStats
  stress + 3 StringPool stress + others including the new
  `ConcurrentDDLDoesNotBreakReaders` and
  `SchemaRetryRePreparesOnSchemaChange`).
- `out/mac_release/perfetto_integrationtests` (TP-relevant
  filter): **122 PASSED**, unchanged.
- `tools/diff_test_trace_processor.py`: **1355 PASSED + 9
  pre-existing skips**, unchanged.
- `out/mac_asan/perfetto_unittests --gtest_filter=
  TraceProcessorConnectionTest.*`: 23/23 PASSED across **10
  consecutive runs**, no ASan reports.
- `out/mac_tsan/perfetto_unittests --gtest_filter=
  TraceProcessorConnectionTest.*`: 23/23 PASSED across **10
  consecutive runs**, no `WARNING: ThreadSanitizer:` output. Full
  TSan suite: 3261 PASSED + 2 SKIPPED + 1 pre-existing macOS
  failure (matching the release-baseline shape).

What's deferred to Phase 4:
- `RuntimeTableFunctionModule` cross-conn (engine-pointer in
  State; carry-over from Phase 2 deferral).
- `StaticTableFunctionModule` cross-conn (Cursor sharing;
  carry-over from Phase 2 deferral).
- Static built-in functions registered post-`InitPerfettoSqlEngine`
  on the writer don't replicate to readers (only dynamic
  CREATE-PERFETTO-FUNCTION entries land in the pool today;
  carry-over from Phase 2 deferral).
- TSan on Linux. macOS arm64 TSan works; the `out/linux_tsan`
  args entry exists in `tools/setup_all_configs.py` but a Linux
  build hasn't been validated in this checkout.
- RPC pool (`trace_processor_rpc.{h,cc}` currently single-engine —
  Phase 4's headline chunk).
- UI engine.ts fan-out (a single `WasmEngineProxy` today; Phase 4
  will mint multiple).
- WASM pthreads — `buildtools/BUILD.gn` does not currently set
  `-pthread` for the wasm toolchain; Phase 4 will need to flip
  that and verify SAB-cross-origin headers in the UI.

Phase 4 entry point is the **RPC pool**: spin up a small fixed
pool of `Connection`s in `TraceProcessorRpc` keyed by query (or
round-robin) so ingestion-thread queries don't serialise behind
each other. The TP-side primitives are now all in place — every
chunk in this phase exercised them under stress, so Phase 4 is
"plumbing higher in the stack" rather than core TP work.

## Phase 4 wrap-up

Branch: `dev/lalitm/multi-conn-tp` — 30 commits ahead of `main` (29
code + this iter-6 wrap-up commit). Phase 4 closes here. The
**multi-connection TraceProcessor initiative is feature-complete**
on the infrastructure axis but the originally stated end-to-end
target — *"visible parallelism in the UI when loading a large
trace"* — is **not met by Phase 4 as shipped**: the realistic
post-EOF UI workload sees a 1.02x speedup. See the headline below
and the three follow-on options.

What shipped across iters 1-5:
- **RPC thread pool** (`rpc-thread-pool`, iter 1): `Rpc::Query`
  fans queries across a `base::ThreadPool` sized to
  `min(hardware_concurrency, 8)`; per-query worker acquires a
  pre-minted `TraceProcessor::Connection` from a free-list pool
  and `DrainConnectionPoolForMutation` drains the pool around any
  writer-side mutation so the
  `non_default_connection_count_ == 0` `PERFETTO_CHECK` cannot
  fire. 4 new `RpcTest.*` tests; TSan + ASan stress clean.
- **UI engine.ts fan-out audit** (`ui-engine-fan-out`, iter 2 —
  **audit-only**): the UI's `streamingQuery` was already not
  serialising at the client level. Audit instead surfaced two
  C++-side gating issues: (a) the websocket transport's
  `TPM_QUERY_STREAMING` case in `Rpc::ParseRpcRequest` called
  `trace_processor_->ExecuteQuery` on the writer engine
  directly — bypassing the iter-1 pool; (b) `MaybeLockFreeTask
  Runner` was single-threaded and `Rpc::Query` blocks via
  `done_fut.wait()`, so even concurrent websocket messages
  serialised on the task-runner thread. Recommended (and
  promoted to ahead of `wasm-pthreads`) the new
  `httpd-pool-dispatch` chunk to unblock both.
- **Httpd pool dispatch** (`httpd-pool-dispatch`, iter 3): `Rpc`
  exposes `SetResponseDispatcher`; httpd wires it at
  construction. `TPM_QUERY_STREAMING` post-EOF queries now
  dispatch onto the iter-1 pool, materialise on a worker, and
  post a single send-all-chunks-for-this-slot closure back via
  the dispatcher. The closure runs on the task-runner thread,
  drains slots in dispatch order (preserving the UI's
  `pendingQueries[0]` FIFO contract), and assigns `tx_seq_id_`s
  in send order. `OnRpcRequest` returns immediately, so
  concurrent websocket messages no longer serialise behind one
  in-flight query. Wasm bridge and `/rpc` HTTP endpoint
  deliberately bypass the async path.
- **End-to-end perf validation** (`e2e-perf-validation`, iter
  4): synthetic Google-Benchmark harness drives
  `Rpc::OnRpcRequest` with vs. without the iter-3 dispatcher.
  Surfaced the BtShared bottleneck (see headline number). Added
  `PERFETTO_RPC_POOL_DISABLED=1` env kill-switch as a v1
  stability gate.
- **WASM pthreads** (`wasm-pthreads`, iter 5): a third
  `wasm_pthreads` gn toolchain emits
  `trace_processor_pthreads.{js,wasm}` alongside the existing
  single-thread + memory64 builds; `wasm_bridge.ts`
  runtime-selects via `self.crossOriginIsolated`. Production
  `ui.perfetto.dev` does NOT currently set COOP+COEP, so the
  pthreads bundle ships but is dormant until a deployment-side
  flip lands.

### Headline performance number

**1.02x on a realistic post-EOF UI query workload; 5.4x on a
CPU-only diagnostic** (`out/mac_release`, 10 cores, 8-worker
pool, `test/data/android_postboot_unlock.pftrace`, median
wall-time over 10 reps). The dispatch / fan-out plumbing is
correct (the 5.4x speedup on a CPU-bound recursive-CTE
workload proves it). The realistic workload sees no gain
because **SQLite's per-`BtShared` mutex serialises btree reads
across connections**: every `Connection` shares the same
`BtShared` because `cache=shared` is on, and the post-EOF UI
query flurry is dominated by btree-backed table reads.

The bottleneck has moved from "no concurrency in TP" to
"BtShared serialisation in SQLite". The Phase 4 ceiling is a
SQLite library constraint, not a Perfetto design defect.

### Three options for unblocking (per iter 4)

1. **Per-connection private cache** — drop `cache=shared`,
   replicate schema some other way (e.g. each connection runs
   its own DDL replay from a shared canonical script). Each
   connection gets its own `BtShared` and the mutex stops
   serialising. Cost: schema replication needs a new mechanism
   (today `cache=shared` carries it for free), and any
   per-`BtShared` page caches are now unique per connection
   (memory grows linearly with connection count).
2. **Dataframe-only post-EOF query path** — many UI queries
   don't need SQLite's planner; the project already has
   `DataframeModule` infra. Bypass SQLite for queries that hit
   only dataframe-backed tables, leaving SQLite for the
   complex cases. Connections never enter btree code, the
   `BtShared` mutex is never taken, and the worker pool gets
   real parallelism.
3. **Fork SQLite's btree to add reader-writer concurrency**
   — invasive, library-level change. Largest engineering cost,
   smallest isolation around the change.

### Recommended Phase 5 entry point

**Option 2 — dataframe-only post-EOF query path.** Lowest risk
(no SQLite library changes; `cache=shared` semantics
preserved for the SQLite path), highest leverage (the UI's
post-EOF query flurry is dominated by table-scan-shaped reads
that the dataframe layer already serves). The project has
`DataframeModule` infra in place. Concretely the Phase 5
entry chunk is: detect at parse / prepare time whether a query
touches only dataframe-backed tables; if so, route it into a
SQLite-bypassing executor that runs against the dataframes
directly under the worker pool. Queries that touch a
non-dataframe vtab fall through to the existing path. Seeded
here as a **Phase 5 candidate**, not a Phase 4 chunk.

### Other deferred items still in scope

Carry-over from earlier phases plus new items surfaced in
Phase 4:
- `RuntimeTableFunctionModule` cross-conn — engine pointer in
  State; needed to enable `CREATE PERFETTO FUNCTION ...
  RETURNS TABLE` on secondary connections. Carry-over from
  Phase 2 iter 4.
- `StaticTableFunctionModule` cross-conn — mechanical
  follow-on. Carry-over from Phase 2 iter 4.
- Static built-in function replication — writer-side
  `InitPerfettoSqlEngine`'s post-init lazy registers don't
  propagate today (only dynamic CREATE-PERFETTO-FUNCTION
  entries land in the function pool). Carry-over from
  Phase 2 iter 5.
- COOP+COEP deployment-side flip
  (`infra/ui.perfetto.dev/appengine/main.py`) to actually
  activate the Phase 4 iter-5 pthreads bundle in production.
- TSan on Linux (currently macOS arm64 only). Carry-over from
  Phase 3 iter 7. The `out/linux_tsan` config exists in
  `tools/setup_all_configs.py` but a Linux build hasn't been
  validated in this checkout.

### Validation at Phase 4 close (re-run on branch tip)

- `out/mac_release/perfetto_unittests`: **3269 PASSED + 2
  SKIPPED + 1 pre-existing macOS failure** (`HttpServerTest.
  Websocket`, matches every previous Phase). 3272 tests across
  352 suites.
- `out/mac_release/perfetto_integrationtests` (TP filter
  `TraceProcessor*:*Sqlite*:ReadTrace*`): **122 PASSED**,
  unchanged.
- `tools/diff_test_trace_processor.py`: **1355 PASSED + 9
  pre-existing skips** (etm + llvm_symbolizer modules absent),
  unchanged.
- `out/mac_asan/perfetto_unittests --gtest_filter=Trace
  ProcessorConnectionTest.*:RpcTest.*`: 30/30 across **10
  consecutive runs**, no ASan reports.
- `out/mac_tsan/perfetto_unittests --gtest_filter=Trace
  ProcessorConnectionTest.*:RpcTest.*`: 30/30 across **5
  consecutive runs**, no `WARNING: ThreadSanitizer:` output.
- `ui/build --typecheck --no-depscheck`: clean.
- `ui/run-unittests --no-depscheck`: clean (matches iter-5
  baseline of 125 suites / 2249 passed / 1 skipped).

The four-phase initiative — phase 1 refactor + memdb, phase 2
multi-conn single-threaded, phase 3 thread safety + retry,
phase 4 RPC pool + UI fan-out + WASM pthreads — is now
infrastructure-complete. A Phase 5 (BtShared bypass via
dataframe-only post-EOF path) can deliver the realised UI
speedup on top of this infrastructure.

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
- [x] busy-retry — done Phase 3 iter 5. `BusyRetryHelper` lives in
      `src/trace_processor/sqlite/sqlite_engine.{h,cc}` and wraps
      both `SqliteEngine::PrepareStatement` (around
      `sqlite3_prepare_v2`) and `SqliteEngine::PreparedStatement::Step`
      (around `sqlite3_step`, with `sqlite3_reset` between retries
      because step leaves the statement indeterminate on BUSY/LOCKED).
      Capped exponential backoff (`100us → 1ms → 10ms → 50ms cap`)
      with default timeout 1s; deadline computed from
      `base::GetWallTimeMs()` at construction. `set_busy_retry_
      timeout_for_testing` exposes a knob; threading through
      `Config` is a follow-up. New `ConcurrentIncludesUnderSharedCache
      NowSucceeds` test lifts the iter-2 pre-include workaround:
      two secondaries from two threads concurrently INCLUDE the
      same never-promoted module and both succeed without
      orchestration. 4 direct `BusyRetryHelperTest.*` unit tests
      cover the helper. 25/25 ASan-clean across 10 consecutive runs.
      See Phase 3 iter 5 activity entry.
- [x] schema-retry — done Phase 3 iter 6. Transparent `SQLITE_SCHEMA`
      recovery in both `SqliteEngine::PrepareStatement` and
      `PreparedStatement::Step`: SCHEMA triggers a finalize +
      `sqlite3_prepare_v2` re-issue from the saved `SqlSource`. New
      `SchemaRetryHelper` (deadline + hard count cap of 100) lives
      next to `BusyRetryHelper` in `sqlite_engine.{h,cc}`. The Step
      path tracks `rows_seen_` and refuses to auto-recover from a
      mid-iteration SCHEMA (cursor cannot be safely restarted). New
      `SqliteEngine::ExecWithRetry` wraps `sqlite3_exec` with the
      same BUSY/LOCKED + SCHEMA semantics and is now used by all six
      savepoint open/release/rollback sites in `PerfettoSqlEngine`,
      so cross-connection DDL no longer leaks "database schema is
      locked: main" through the savepoint boundary either. New
      stress test `ConcurrentDDLDoesNotBreakReaders` (writer churns
      100 CREATE/DROP pairs, reader does 200 SELECTs on a sibling)
      is clean across 10 consecutive ASan runs. See Phase 3 iter 6
      activity entry.
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
- [x] string-pool-thread-safety — done Phase 3 iter 4.
      `TraceProcessorImpl::CreateConnection` now flips
      `StringPool::EnableThreadSafetyForMultiConnection()` (a new
      public no-arg method that wraps the existing
      `should_acquire_mutex_` setter under a deliberately awkward
      name to prevent flipping back to off) on the shared
      `TraceStorage` pool *before* returning the secondary
      connection. The existing `MaybeLockGuard` covered every
      mutating + iterating method already; no further locking
      changes were needed inside `string_pool.{h,cc}`. Single-
      connection callers never reach `CreateConnection` so they
      continue to pay zero locking overhead. Three new tests:
      `StringPoolTest.ConcurrentInternIsThreadSafe` (8 threads,
      2000 iters each, direct pool stress) plus
      `TraceProcessorConnectionTest.{ConcurrentInternFromMultiple
      Connections, InternedStringMatchesAcrossConnections}` —
      29/29 ASan-clean across 10 consecutive runs. See Phase 3
      iter 4 activity entry.
## Next chunks (Phase 4 — first cut, refine on /loop restart)

- [x] rpc-thread-pool — done Phase 4 iter 1. `Rpc` fans `Query` RPCs
      across a `base::ThreadPool` sized to
      `min(hardware_concurrency, 8)`; per-query worker acquires a
      pre-minted `TraceProcessor::Connection` from a lazy free-list
      pool, drain-then-releases (option (a) — iterator destructed
      before connection returns to the pool, since otherwise a peer
      worker can re-acquire mid-finalize and race on the underlying
      `sqlite3*`), then streams the buffered chunks back to the
      transport. Mutating RPCs (`Parse` via `ResetTraceProcessor
      Internal`, `RegisterSqlPackage`, `RestoreInitialTables`) drain
      the pool first so the writer-side
      `non_default_connection_count_ == 0` `PERFETTO_CHECK` gate
      cannot fire. Connections are pre-minted from the (single)
      caller thread of the first post-EOF Query — workers never
      call `CreateConnection` — to avoid a TSan-flagged race on
      `StringPool::should_acquire_mutex_` (single-producer
      contract for the bool flip). 4 new tests under
      `RpcTest.*`. See Phase 4 iter 1 activity entry.
- [x] ui-engine-fan-out — done Phase 4 iter 2 (audit-only). The UI
      already does NOT serialise `query()` calls: `streamingQuery`
      pushes a `WritableQueryResult` onto `pendingQueries` and
      immediately calls `rpcSendRequestBytes`, with no client-side
      mutex / promise-chain in between. The bottleneck for
      HTTP-RPC parallelism is in C++, not in `engine.ts`: the
      websocket path routes through
      `Rpc::OnRpcRequest → ParseRpcRequest → TPM_QUERY_STREAMING`
      (rpc.cc:278-331), which calls `trace_processor_->ExecuteQuery`
      directly on the writer engine. It does **not** go through
      `Rpc::Query` — the only path wired to the iter-1 worker pool.
      Even if it did, `httpd.cc`'s `task_runner_` is single-threaded
      (one `MaybeLockFreeTaskRunner` per `Httpd`) and `Rpc::Query`
      blocks the caller via `done_fut.wait()`, so concurrent UI
      queries would still serialise on the task-runner thread.
      Added a comment to `engine.ts` documenting the FIFO
      response-matching contract (the sole dependency the UI has
      on the C++-side ordering — chunks for one query must be
      emitted contiguously on the wire, which `Rpc::Query` already
      honours per-call). No functional change. See the Phase 4
      iter 2 activity entry for the full audit.
- [x] httpd-pool-dispatch — done Phase 4 iter 3. `Rpc` now exposes
      `SetResponseDispatcher`; when wired (httpd does so at
      construction), `ParseRpcRequest`'s `TPM_QUERY_STREAMING` case
      dispatches post-EOF queries onto the iter-1 worker pool.
      A worker materialises chunks and posts a single
      "send-all-chunks-for-this-slot" closure back via the
      dispatcher. The closure runs on the task-runner thread, drains
      ready slots in dispatch order (preserving the UI's
      `pendingQueries[0]` FIFO invariant), and assigns
      `tx_seq_id_`s in send-order. `OnRpcRequest` returns
      immediately, so concurrent websocket messages no longer
      serialise behind one in-flight query. The wasm bridge and
      `/rpc` HTTP endpoint deliberately bypass the async path
      (wasm doesn't set a dispatcher; `/rpc` swaps the dispatcher
      out around its `OnRpcRequest` because its chunked-transfer
      trailer is sent immediately after `OnRpcRequest` returns).
- [x] wasm-pthreads — done Phase 4 iter 5. Build infrastructure:
      a new `wasm_pthreads` gn toolchain compiles the trace
      processor with `-pthread` and links with `-s
      PTHREAD_POOL_SIZE=8`, producing a third wasm artifact
      (`trace_processor_pthreads.{js,wasm,d.ts}`) alongside the
      existing single-thread and memory64 builds. Runtime fallback:
      `ui/src/engine/wasm_bridge.ts` picks the pthreads module iff
      `self.crossOriginIsolated === true` and SharedArrayBuffer is
      reachable, else falls through to the single-thread module
      unchanged. Deployment dependency: `ui.perfetto.dev` does NOT
      currently set COOP+COEP (verified in
      `infra/ui.perfetto.dev/appengine/main.py`), so production
      stays on the single-thread variant; the pthreads bundle ships
      but is dormant until a separate deployment-side flip lands.
      Caveat: speedup remains capped by the iter-4 BtShared finding
      (1.02x on real trace-table workloads) — this iter lands the
      infrastructure that gates the future shared-cache fix on a
      working multi-thread sandbox, not the speedup itself. A
      separate Phase 4 wrap entry should follow that closes the
      loop without further code chunks.
- [x] e2e-perf-validation — done Phase 4 iter 4. Synthetic
      Google-Benchmark harness (`rpc_perf_benchmark.cc`) drives
      `Rpc::OnRpcRequest` with vs. without the iter-3 dispatcher.
      Headline numbers (mac_release, 10 cores, 8-worker pool, 10
      reps, median wall-time): trace-table workload **196ms vs
      199ms (1.02x)**; CPU-only recursive-CTE workload **7.81ms vs
      42.4ms (5.4x)**. The dispatch / fan-out plumbing is correct
      (5.4x speedup proves it); the trace-table workload sees no
      gain because **SQLite's shared-cache `BtShared` mutex
      serialises btree reads across connections**. Documented as
      the v1 parallelism ceiling for the http-rpc transport. Added
      `PERFETTO_RPC_POOL_DISABLED=1` env kill-switch as v1
      stability gate. See Phase 4 iter 4 activity entry for the
      full bottleneck investigation and the three follow-on options
      (private-cache / dataframe-only / fork SQLite btree).

- [x] tsan-multithread-stress — done Phase 3 iter 7.
      `out/mac_tsan` (`is_clang=true is_debug=false is_tsan=true`)
      builds and runs `perfetto_unittests` cleanly on macOS arm64 —
      `tools/setup_all_configs.py` already advertised the config and
      `buildtools/BUILD.gn` had the `is_tsan` plumbing, the build dir
      simply hadn't been instantiated. The first TSan run flagged a
      real race in `TraceProcessorImpl::ReleaseConnection`:
      `non_default_connection_count_` was a plain `int` decremented
      from connection destructors, which the
      `ConcurrentRecordingIntoSqlStats` test happens to fire from
      multiple threads when the per-thread `unique_ptr<Connection>`
      goes out of scope. Promoted the counter to `std::atomic<int>`
      with `fetch_add(1, relaxed)` on create and
      `fetch_sub(1, acq_rel)` on release (returning the prior value
      so the underflow guard CHECKs strictly-positive). Read sites
      (the writer-thread `PERFETTO_CHECK(... == 0)` mutation gates)
      keep their existing form — `std::atomic<int>::operator==(int)`
      does an implicit relaxed load and the expected value at those
      sites is zero with a single producer. After the fix, 10/10
      stress runs of the full `TraceProcessorConnectionTest.*` suite
      under TSan are clean and the full suite runs at 3261 PASSED +
      2 SKIPPED + 1 pre-existing macOS failure
      (`HttpServerTest.Websocket`), matching the release baseline
      shape.

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
