# Android performance tutorials

A series of tutorials walking through the major shapes of Android
performance investigation in Perfetto. Each tutorial:

- Ships a small reproducible demo app (buggy + fixed builds).
- Includes a Perfetto trace config that captures exactly what the
  doc references.
- Shows headline screenshots from a captured trace, with concrete
  numbers from the demo.
- Ends with a verify step backed by a second screenshot of the
  fixed trace.
- Has a paired `artifacts/` subdirectory that anyone can re-run to
  regenerate every screenshot.

For the artifact pattern in detail, and the source-line → screenshot
map for each tutorial, see the artifacts branch:
<https://github.com/fiveapplesonthetable/perfetto/tree/perf-tutorials-artifacts>.

## Available

- [Frame jank](frame-jank.md) — `RecyclerView`/`ListView` adapter
  doing synchronous bitmap decode on the UI thread. Frame timeline
  + main-thread track. **Heap graph not needed.**

## Planned

In rough sequencing order — earlier topics teach UI primitives
(frame timeline, sched tracks, atrace categories) that later ones
re-use:

1. App startup (cold/warm) — `Application.onCreate` synchronously
   initializing several SDKs in series. App Startup library + lazy
   init.
2. Binder spam — calling a `ConnectivityManager` API from
   `onPreDraw`. `ftrace binder/*` + callstack sampling.
3. Lock contention — `synchronized` cache hammered from a 16-thread
   pool. ART `Lock contention` slices.
4. Main-thread I/O — `SharedPreferences.commit` in `onResume`.
   `f2fs/*` ftrace + sched.
5. Java heap allocations over time — search screen allocating a
   fresh `ArrayList` per keystroke. `java_hprof_config` sampler.
6. Native heap — JNI bridge with leaked `malloc` on the error path.
   `heapprofd`.
7. GC pauses — string concatenation in a hot loop. ART `GC` slices
   + frame timeline.
8. CPU spinning — hand-rolled JSON parser with O(n²) substring.
   `linux.perf` callstack sampling.
9. Short-lived thread spam — `Thread { }.start()` per item.
   `task/task_newtask` ftrace.
10. Wakelocks / 24-hour battery — `LocationManager` request without
    `removeUpdates`; `JobService` that never calls `jobFinished`.
    Long-trace battery config.

Each tutorial follows the same shape as the [Heap Dump
Explorer](/docs/visualization/heap-dump-explorer.md) doc: capture →
read the trace → fix → verify, with two case studies per topic
showing different surface shapes of the same bug class.

## Contributing a tutorial

Each tutorial gets its own subdirectory under `artifacts/<topic>/`
in the artifacts branch with this layout:

```
artifacts/<topic>/
├── README.md            one-shot reproduction + source-line → screenshot map
├── demo-buggy/          smallest app that reproduces the bug
│   ├── AndroidManifest.xml
│   ├── build.sh         AOSP prebuilts only, no Gradle
│   └── src/...
├── demo-fixed/          same app, fix applied
│   └── src/...
├── trace-configs/       textproto Perfetto configs
├── traces/              before.pftrace / after.pftrace
└── playwright/          shoot.js for screenshots
```

Hard rules for new tutorials:

- The bug must fire deterministically within a few seconds of
  `am start`, no manual interaction beyond at most one tap.
- The bug must be small — one file, ideally under 50 lines. Readers
  point at the bad line in a single screenshot.
- Buggy and fixed apps share package, Activity names, and UI — only
  the bad code path swaps. The doc's verify step becomes a one-line
  diff and a paired before/after screenshot.
- The trace config is checked in as a textproto, not described in
  prose.
- The Playwright shooter is idempotent: same trace in, same images
  out. Set `localStorage.cookieAck`, pin viewport, drive navigation
  via deep-link URL hashes.
- The artifact README maps each source line of the bug to the
  screenshot that visualises it. Reviewers verify the artifact PR
  without opening the doc PR.
- Two case studies per doc, not one — the reader learns the
  technique, not the example. Different surface shape, same trace
  view.
