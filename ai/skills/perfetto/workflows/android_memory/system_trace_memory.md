---
related:
  - path: system_trace_memory_compare.md
    when: Compare two traces (before/after)
  - path: recording_memory_signals.md
    when: Recording the missing signals
  - path: heap_dump_diff.md
    when: Compare two Java heap dumps (before/after diff)
  - path: heap_profile_diff.md
    when: Compare two native or Java heap profiles (before/after diff)
---
# Android Memory in System Traces

Investigates device-wide and per-process memory in **regular system traces**
(counters, atrace and ftrace events): memory spikes or steady growth, Java heap
and GC behaviour, graphics and DMA-BUF buffers, swap and memory pressure, and
low-memory kills, in one trace or across a `before` / `after` pair.

System traces show **how much** memory moved and **when**, not **which objects
or callstacks** own it. Hand off to the profiler workflows for that:
[heap_dump.md]($SKILL_ROOT/workflows/android_memory/heap_dump.md)
(retained Java objects),
[java_allocation_profile.md]($SKILL_ROOT/workflows/android_memory/java_allocation_profile.md)
(Java churn), and
[native_heap.md]($SKILL_ROOT/workflows/android_memory/native_heap.md)
(native heap).

## Step 1: Load the trace(s) and check what they recorded

Load each trace once into a warm session and query it by name:

```bash
trace_processor server unix --name tp --daemonize '<trace>'
trace_processor query --remote tp "<SQL>"  # or -f query.sql
# when done: trace_processor server kill tp
```

**Before any analysis**, check that each trace recorded the signals the
question needs. Most memory signals come from data sources, ftrace events or
atrace categories that are off unless the trace config enables them, and a
signal that was never recorded looks exactly like one that never changed.
Read the config embedded in the trace, keeping only the lines that matter
(full configs can be tens of KB):

```bash
trace_processor query --remote tp \
  "SELECT str_value FROM metadata WHERE name = 'trace_config_pbtxt'" |
  grep -E 'linux\.(process|sys)_stats|android\.(gpu\.memory|heapprofd|java_hprof)|atrace_categories|atrace_apps: "(lmkd|\*)"|_period_ms|_poll_ms|rss_stat|oom_score_adj|gpu_mem|dma_heap|lowmemory' |
  sort -u
```

and check it against what each signal needs:

| Signal | Needs in the config |
| --- | --- |
| `mem.rss`, `mem.rss.*`, `mem.swap` (polled) | `linux.process_stats` (`proc_stats_poll_ms` for periodic samples; without it, usually only at trace start and end) |
| `mem.rss.anon` / `.file` / `.shmem`, `mem.swap` on change | ftrace `kmem/rss_stat` (with `throttle_rss_stat: true`) |
| `oom_score_adj` on change | ftrace `oom/oom_score_adj_update` |
| `meminfo`, `vmstat`, `psi` | `linux.sys_stats` with `meminfo_period_ms`, `vmstat_period_ms`, `psi_period_ms` |
| `Heap size (KB)`, GC slices, `JNI Global Refs` | atrace category `dalvik` |
| `Bitmap *`, `mem.gralloc.*`, `surfaceflinger` buffers, window queues | atrace category `gfx` |
| `*HWUI*`, `Skia *` | atrace category `view` |
| `binder_proxies` | atrace category `aidl` |
| `oom_adj_reason` | atrace category `am` |
| `GPU Memory` | `android.gpu.memory` data source and ftrace `gpu_mem/gpu_mem_total` |
| `mem.dma_heap`, `android_dmabuf_allocs` | ftrace `dmabuf_heap/dma_heap_stat` (plus binder tracing to attribute gralloc allocations) |
| `mem.dmabuf_rss` | ftrace `kmem/dmabuf_rss_stat`, or `record_process_dmabuf_rss: true` in `linux.process_stats` |
| LMK kills | `atrace_apps: "lmkd"` (or `"*"`); ftrace `lowmemorykiller/lowmemory_kill` on kernels with the in-kernel driver |
| Heap profile, heap dump | `android.heapprofd`, `android.java_hprof` |

Ftrace events may be listed without their group (`"rss_stat"`).

- **Not in the config**: the trace cannot answer anything that depends on
  that signal. **Tell the user up front**, before reporting other results,
  which signals are missing and which parts of the question they block, and
  suggest recording a new trace with them. Never read a missing signal as
  zero or "no change".
- **In the config, but the process has no such track**: the source was on and
  the process emitted nothing (for example no change of a change-driven
  counter, no GC, no frames drawn); see [Signal reference](#signal-reference).
- **No config** (empty result: older traces, converted systrace or JSON):
  check whether the tracks exist for any process
  (`SELECT DISTINCT name FROM counter_track`); a counter present for one
  process proves its source was on.
- An enabled event can still fail on a given kernel or build. If
  `SELECT value FROM stats WHERE name = 'ftrace_setup_errors'` is above zero,
  list the failures with `SELECT EXTRACT_ARG(arg_set_id, 'message') FROM
  _trace_import_logs WHERE name = 'ftrace_setup_errors'` and treat those
  events as missing.

## Step 2: Resolve the target process

Resolve the process to a `upid` **in each session** and scope every later query
by `upid`, not by name. Short-lived children forked from an app keep its name
(for example `libmemunreachable` scans behind `dumpsys meminfo --unreachable`,
or heap dump forks), and a restarted process gets a new `upid`:

```sql
SELECT
  p.upid, p.pid, p.start_ts, p.end_ts,
  pp.name AS parent,
  (SELECT COUNT(*) FROM thread AS t WHERE t.upid = p.upid) AS threads
FROM process AS p
LEFT JOIN process AS pp ON pp.upid = p.parent_upid
WHERE p.name = '<PROCESS>';
```

The real app is normally the row whose parent is `zygote64` / `zygote` with
many threads; report a restart (several such rows) instead of merging them.

## Step 3: Find where each signal lives

Query only the signals relevant to the question and recorded in Step 1. List
the process's tracks first (`SELECT name FROM process_counter_track WHERE upid =
<UPID>`; device-wide: `SELECT type, name FROM counter_track WHERE type IN
(...)`). **Before drawing a conclusion from a signal, read
its entry in [Signal reference](#signal-reference)**: most of these counters
are emitted only on change, and several have no value at trace start.

| Question | Where to look |
| --- | --- |
| Device memory pressure | `counter_track` of `type` `meminfo` (`MemAvailable`, `SwapFree`, `AnonPages`, ...), `vmstat` (`pswpin`, `pswpout`, `pgfault`, `nr_free_pages`), `psi` (`psi.mem.some`, `psi.mem.full`) |
| Process RSS and swap | `process_counter_track`: `mem.rss.anon`, `mem.swap`, `mem.rss.file`, `mem.rss.shmem`, `mem.rss.watermark`; stdlib `android.memory.process` → `memory_oom_score_with_rss_and_swap_per_process` (`anon_rss_and_swap`, `score`; filter by `upid`) |
| Java heap and GC | `Heap size (KB)` counter; GC slices `<cause> <collector> GC` on any thread; `GC: Wait For Completion <cause>` slices; `freed_bytes` counters |
| Bitmaps | `Bitmap Count`, `Bitmap Memory` (stdlib `android.bitmaps`) |
| GPU and graphics memory | per-process `GPU Memory`, `Skia * GPU Bytes`, `*HWUI*`, `mem.gralloc.buffers` plus the `mem.gralloc.allocations` instant track, `mem.dmabuf_rss`; device-wide `counter_track` `type` `gpu_memory` and `android_dma_heap` (`mem.dma_heap`); `surfaceflinger` `Total Buffer Size`, `RE VMA-*` |
| DMA-BUF allocations | stdlib `android.memory.dmabuf` → `android_dmabuf_allocs` (`buf_size` > 0 alloc, < 0 free, `process_name`) |
| Window surfaces | `QueuedBuffer - VRI[<window>]...` and `VRI[<window>](BLAST Consumer)N` |
| Binder and JNI references | `binder_proxies`, `JNI Global Refs` |
| Process state and LMK | stdlib `android.oom_adjuster` → `android_oom_adj_intervals` (`score`, `bucket`, `oom_adj_reason`); `android.memory.lmk` → `android_lmk_events` (`process_name`, `oom_score_adj`, `kill_reason`) |
| Embedded profiles | `SELECT heap_name, COUNT(*) FROM heap_profile_allocation WHERE upid = <UPID> GROUP BY heap_name` (`com.android.art` / `libc.malloc`); `heap_graph_object` (`upid = <UPID>`). Check `stats` (`heapprofd_buffer_overran`) before trusting completeness; if intact, run the profiler workflows on the same session. |
| Lost data | `stats` where `severity IN ('error', 'data_loss') AND value > 0` (`heapprofd*`, `ftrace*`, `traced_buf*`), plus the `info`-level `rss_stat_unknown_thread_for_mm_id` |

## Step 4: Summarize counters and GC activity

### Counter summary

Returns `start_val`, `min_val`, time-weighted `avg_val`, `max_val`, `end_val`,
`net_delta` (`end_val - start_val`) and the raw sample count per track.
Selecting the tracks first lets the `counter` scan use its `track_id` index
(well under a second on a 45 MB trace). Widen or drop the name filter as needed:

```sql
INCLUDE PERFETTO MODULE counters.intervals;

WITH
  tracks AS MATERIALIZED (
    SELECT id, name
    FROM process_counter_track
    WHERE upid = <UPID>
      AND (
        name GLOB 'mem.*'
        OR name IN ('Heap size (KB)', 'GPU Memory', 'Bitmap Count', 'Bitmap Memory',
                    'binder_proxies', 'JNI Global Refs', 'oom_score_adj')
        OR name GLOB '*HWUI*'
        OR name GLOB 'Skia *'
      )
  ),
  spans AS (
    SELECT
      track_id, dur, value,
      FIRST_VALUE(value) OVER w AS start_val,
      LAST_VALUE(value) OVER w AS end_val
    FROM counter_leading_intervals!((
      SELECT c.id, c.ts, c.track_id, c.value
      FROM counter AS c
      JOIN tracks AS t ON c.track_id = t.id
    ))
    WINDOW w AS (
      PARTITION BY track_id ORDER BY ts
      ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
    )
  )
SELECT
  t.name AS counter,
  CAST(MAX(s.start_val) AS INT) AS start_val,
  CAST(MIN(s.value) AS INT) AS min_val,
  CAST(SUM(s.value * s.dur) / NULLIF(SUM(s.dur), 0) AS INT) AS avg_val,
  CAST(MAX(s.value) AS INT) AS max_val,
  CAST(MAX(s.end_val) AS INT) AS end_val,
  CAST(MAX(s.end_val) - MAX(s.start_val) AS INT) AS net_delta,
  (SELECT COUNT(*) FROM counter AS c WHERE c.track_id = t.id) AS samples,
  (SELECT MIN(c.ts) FROM counter AS c WHERE c.track_id = t.id) - trace_start()
    AS first_sample_offset_ns
FROM spans AS s
JOIN tracks AS t ON s.track_id = t.id
GROUP BY t.id
ORDER BY t.name;
```

`first_sample_offset_ns` shows how far into the trace a change-driven counter
first reported; the counter has no known value before then.

For device-wide counters, replace the `tracks` CTE body with
`SELECT id, name FROM counter_track WHERE type IN ('meminfo', 'vmstat', 'psi',
'gpu_memory', 'android_dma_heap')`.

### GC events with the Java heap before and after each one

ART reports the reduced `Heap size (KB)` either **inside** the collection
(including any immediately following `CompactionPhase` slice on the same thread)
or as the first sample right at its end, while later samples already include new
allocations; take the minimum across both:

```sql
INCLUDE PERFETTO MODULE slices.with_context;

WITH
  gc_slices AS (
    SELECT
      s.upid,
      s.ts,
      IIF(s.dur = -1, trace_end() - s.ts, s.dur) AS dur_ns,
      s.thread_name,
      s.name,
      s.ts + IIF(s.dur = -1, trace_end() - s.ts, s.dur) + IIF(
        LEAD(s.name) OVER (PARTITION BY s.track_id, s.depth ORDER BY s.ts)
          = 'CompactionPhase',
        LEAD(s.dur) OVER (PARTITION BY s.track_id, s.depth ORDER BY s.ts),
        0
      ) AS gc_end
    FROM thread_slice AS s
    WHERE s.upid = <UPID>
      AND (s.name GLOB '* GC'
           OR s.name GLOB 'GC: Wait For Completion*'
           OR s.name = 'CompactionPhase')
  )
SELECT
  s.ts,
  s.thread_name,
  s.name,
  ROUND(s.dur_ns / 1e6, 1) AS dur_ms,
  (SELECT CAST(c.value AS INT)
   FROM counter AS c
   JOIN process_counter_track AS t ON c.track_id = t.id
   WHERE t.upid = s.upid AND t.name = 'Heap size (KB)' AND c.ts <= s.ts
   ORDER BY c.ts DESC LIMIT 1) AS heap_kb_before,
  (SELECT CAST(MIN(v) AS INT) FROM (
     SELECT MIN(c.value) AS v
     FROM counter AS c
     JOIN process_counter_track AS t ON c.track_id = t.id
     WHERE t.upid = s.upid AND t.name = 'Heap size (KB)'
       AND c.ts BETWEEN s.ts AND s.gc_end
     UNION ALL
     SELECT * FROM (
       SELECT c.value AS v
       FROM counter AS c
       JOIN process_counter_track AS t ON c.track_id = t.id
       WHERE t.upid = s.upid AND t.name = 'Heap size (KB)'
         AND c.ts >= s.gc_end
       ORDER BY c.ts LIMIT 1
     )
   )) AS heap_kb_after
FROM gc_slices AS s
WHERE s.name != 'CompactionPhase'
ORDER BY s.ts;
```

`GC: Wait For Completion` rows are waits, not collections: their heap columns
describe the GC they waited for.

## Step 5: Interpret and route

Report concrete numbers for each counter: `start -> peak -> end` within a trace
(`start_val`, `max_val`, `end_val`; a peak far above both ends is a transient
the end value hides), `before -> after` across traces. Keep measurements
separate from hypotheses.

- **Change-driven counters.** A missing track or a late first sample means
  "nothing changed while tracing", not zero. A track with `samples = 1` shows a
  level, not a trend, and `start_val` is the first report, not the value at
  trace start. High `max_val` with `net_delta ≈ 0` is a transient spike.
- **Java retention vs churn.** Compare `heap_kb_after` across GCs, or across
  the two traces:
  - Rising post-GC heap → objects survive GC (**retention**): use
    [heap_dump.md]($SKILL_ROOT/workflows/android_memory/heap_dump.md).
  - Flat post-GC heap but higher `Heap size (KB)` peaks, more GCs, or
    `GC: Wait For Completion` stalls on app threads → temporary allocation
    **churn**: use
    [java_allocation_profile.md]($SKILL_ROOT/workflows/android_memory/java_allocation_profile.md).
  - No GC in a trace → the minimum of `Heap size (KB)` is not a live-heap
    floor; comparing it with a trace that did collect invents a retention
    delta.
  - Tooling-requested `Explicit ... GC`s (see the GC entry below) give the
    cleanest live-heap reading, so compare those first.
- **Anonymous memory.** `mem.rss.anon` + `mem.swap` covers the Java heap,
  native `malloc`, thread stacks, the JIT cache and anonymous `mmap`s. Compare
  `anon_rss_and_swap`, since reclaim moves pages from `mem.rss.anon` to
  `mem.swap` without changing the footprint. Growth beyond the Java heap and
  `Bitmap Memory` points at native memory:
  [native_heap.md]($SKILL_ROOT/workflows/android_memory/native_heap.md).
  `heapprofd` does not see stacks, the JIT cache or direct `mmap`s.
- **GPU, gralloc and DMA-BUF memory is invisible to heap profilers**, and the
  graphics counters overlap (the same buffer can appear in `Bitmap Memory`,
  `GPU Memory`, Skia / HWUI counters, `mem.dmabuf_rss` and gralloc counts), so
  compare each counter across runs rather than adding them. For new graphics
  buffers, read the `mem.gralloc.allocations` instants to see who requested
  them.
- **Lagging counters.** `Bitmap *` and `binder_proxies` drop only after the
  owning Java object is collected, so they can rise between GCs without a leak;
  check whether they return after a GC before calling it retention.

## Signal reference

**Java heap and GC (ART, atrace `dalvik`)**

- `Heap size (KB)`: Java heap bytes allocated and not yet reclaimed, in KiB.
  It counts whole thread-local allocation buffers (TLABs) handed to threads,
  so it moves in TLAB-sized steps rather than per object, and it is not the
  heap's capacity, its RSS, or `dumpsys meminfo`'s Dalvik PSS. ART reports it
  only while tracing and only on change: on allocation when the value grew by
  at least 16 KiB since the last report, and after each GC frees memory. There
  is **no initial sample**: a process that neither allocates enough nor runs a
  GC during the trace has no track at all (often most processes), and the
  first sample of an active one can arrive hundreds of milliseconds in. With
  the concurrent-copying collector it also includes objects being evacuated,
  so it briefly rises during a GC.
- GC slices: `<cause> <collector> GC`, for example
  `Background young concurrent mark compact GC`, `Alloc ... GC`,
  `NativeAlloc ... GC`, `Explicit ... GC`. Background GCs run on
  `HeapTaskDaemon`; `Alloc` and `Explicit` GCs run on the requesting thread,
  often nested under other slices. `Explicit` GCs are requested
  (`System.gc()`), not triggered by allocation pressure; in test traces they
  usually come from tooling: on a binder thread under
  `IApplicationThread::dumpMemInfo` (`dumpsys meminfo` collects a GC before
  counting objects) or on `Signal Catcher` (`SIGUSR1`). The stdlib `android.garbage_collection` table only keeps
  top-level `*concurrent*GC` slices and misses nested ones, so match slice
  names directly.
- `GC: Wait For Completion <cause>`: a thread blocked until a running GC
  finished. On an app's main or render thread, this is user-visible stall time.
- `freed_bytes`, `freed_normal_object_bytes`, `freed_large_object_bytes`:
  bytes freed by the most recent GC, not a running total.

**Process RSS, swap and state (`linux.process_stats`, `kmem/rss_stat`)**

- `mem.*` tracks mix two sources. `linux.process_stats` polls
  `/proc/<pid>/status` (`mem.rss` = `VmRSS`, `mem.rss.anon`, `mem.rss.file`,
  `mem.rss.shmem`, `mem.swap` = `VmSwap`, `mem.locked`, `mem.virt` = `VmSize`,
  `mem.rss.watermark` = `VmHWM`), often only at trace start and end. The
  `rss_stat` ftrace event pushes `mem.rss.anon`, `mem.rss.file`, `mem.swap` and
  `mem.rss.shmem` on change; on kernels newer than Android 12 it is replaced by
  the rate-limited `rss_stat_throttled` (`throttle_rss_stat: true`). So
  `mem.rss.anon` is usually dense while `mem.rss` is not, and
  `mem.rss` is not the sum of the components at an arbitrary timestamp.
  `rss_stat_unknown_thread_for_mm_id` in `stats` counts dropped updates.
- `mem.rss.watermark` is the peak RSS since the process started (or since it
  was last reset, e.g. by `rss_hwm_reset`), not the peak during the trace.
- `mem.virt` is reserved address space; ART reserves gigabytes, so ignore it
  for memory use.
- `mem.swap` is the uncompressed size of pages swapped to zram; the RAM it
  costs is smaller.
- `oom_score_adj`: polled, plus `oom/oom_score_adj_update` events.
  `oom_adj_reason` in `android_oom_adj_intervals` comes from `system_server`
  `updateOomAdj_*` slices (atrace `am`) and is empty without them. Persistent
  system processes such as SystemUI stay at -800 throughout.

**Device memory (`linux.sys_stats`)**

- `meminfo` counters are polled at `meminfo_period_ms` and converted to bytes.
  `MemAvailable` is the kernel's estimate of memory available without
  swapping.
- `vmstat` counters named `pg*` / `psw*` (`pgfault`, `pswpin` / `pswpout` =
  pages swapped in / out) are event counts since boot, so use `end - start`;
  `nr_*` counters (`nr_free_pages`, ...) are current page counts.
- `psi.*` counters are cumulative stall time in ns since boot: `some` = at least
  one task stalled on that resource, `full` = all non-idle tasks stalled.
  Divide the `end - start` delta by the window length for a stall ratio.

**Graphics and buffers**

- `Bitmap Count` / `Bitmap Memory` (hwui, atrace `gfx`): running totals of
  every native `Bitmap` in the process, whatever backs its pixels (`malloc`
  heap, ashmem, hardware buffer, or a wrapped pixel ref), reported only when a
  bitmap is created or destroyed. A shared ashmem or
  hardware bitmap is counted once per process. A bitmap leaves the total only
  when its native object is destroyed (`recycle()` or after its Java object is
  collected).
- `*HWUI*` (hwui, atrace `view`): reported every frame the process renders.
  `HWUI All Memory` sums the HWUI CPU and GPU cache categories; `Purgeable *`
  tracks are the part that can be released under pressure. Processes that do
  not draw produce no samples.
- `Skia {Protected,Unprotected} * GPU Bytes`: process-wide Skia GPU resources.
  `Total` = `Backend Texture` + `Unbudgeted` + `Budgeted` (non-purgeable) +
  `Purgeable`. Growth only in `Purgeable` is cache, not a leak.
  `Backend Texture` is client-owned textures created through Skia's
  backend-texture API, outside the cache budget.
- `GPU Memory`: the GPU driver's per-process total (global total on the
  `gpu_memory` device track), from the `gpu_mem/gpu_mem_total` ftrace event on
  each change plus one snapshot at trace start from the `android.gpu.memory`
  data source. With only the data source you get that single snapshot.
- `mem.gralloc.buffers` (libui, atrace `gfx`): **count**, not bytes, of
  gralloc buffers this process allocated and has not freed. Buffers imported
  from other processes are not counted. Every allocation and free is also an
  instant on the process track `mem.gralloc.allocations`, named
  `[<requestor>] <width>x<height> - <handle>` or `free - <handle>`. The
  requestor (a window's `VRI[...]` BLAST consumer, `AHardwareBuffer pid [N]`)
  attributes each buffer.
- `mem.dmabuf_rss`: DMA-BUF bytes mapped by the process (`dmabuf_rss_stat`
  event or `/proc/<pid>/dmabuf_rss`). Shared buffers count in every process
  that maps them, so do not sum across processes.
- `mem.dma_heap`: device-wide bytes allocated from DMA-BUF heaps, updated on
  each `dmabuf_heap/dma_heap_stat` event alongside a per-thread delta on
  `mem.dma_heap_change`. It has no value until the first event, whose
  `mem.dma_heap` sample already includes that event's `mem.dma_heap_change`
  delta (so the net device change across the trace is `SUM(mem.dma_heap_change)`,
  not just `end - start` of `mem.dma_heap`). `android_dmabuf_allocs` attributes
  events by matching each free's `inode` to an earlier allocation inside the
  trace, so frees of buffers allocated before tracing started are dropped —
  inflating `SUM(buf_size)` when pre-existing buffers are released.
- `surfaceflinger` `Total Buffer Size`: bytes of buffers allocated by
  SurfaceFlinger, reported each composition. `RE VMA-Allocated` /
  `RE VMA-Used`: RenderEngine Vulkan memory.
- `QueuedBuffer - VRI[...]` / `VRI[...](BLAST Consumer)N`: buffers queued in a
  window's BLAST queue (counts, not bytes); a window name present in only one
  trace is an extra surface.

**Binder and JNI**

- `binder_proxies` (atrace `aidl`): number of outgoing binder proxies
  (`BpBinder`) in the process, reported on every create and destroy. Java
  `BinderProxy` objects release theirs only when collected.
- `JNI Global Refs` (atrace `dalvik`): JNI global references, including those
  backing Java `Binder` objects passed to other processes (`ROOT_JNI_GLOBAL` in
  heap dumps). ART reports it once every 17 additions or deletions, so it is
  only accurate to about ±17.

**Low-memory kills**

- `android_lmk_events` reads `lmkd` events (atrace app `lmkd`): the
  `lowmemorykiller` process track on recent releases, `lmk,...` slices on older
  ones, and the deprecated `kill_one_process` counter (no `kill_reason`) on the
  oldest. Kills from the legacy in-kernel driver (`lowmemorykiller/lowmemory_kill`)
  appear instead as `mem.lmk` rows in the `instant` table.

## Reference

- Memory counters and events: <https://perfetto.dev/docs/data-sources/memory-counters>
- Standard library: <https://perfetto.dev/docs/analysis/stdlib-docs>
