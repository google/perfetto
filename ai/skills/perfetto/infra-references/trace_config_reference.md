# Synthesizing Perfetto Trace Configs (Mix & Match)

Read this when you need a custom trace config — a mixture of data sources
the specialized helper scripts don't cover. Start from the exemplar configs
in `$SKILL_ROOT/infra-references/example-configs/`, then add, remove, or
merge the pieces described below.

## Shape of a config

A config is a `TraceConfig` protobuf — authoritative schema:
<https://raw.githubusercontent.com/google/perfetto/main/protos/perfetto/config/trace_config.proto>
(per-source option messages live under `protos/perfetto/config/` in the same
repo, e.g. `.../ftrace/ftrace_config.proto`) — written in protobuf **text
format** (conventionally saved as `config.pftxt`):

```
buffers {
  size_kb: 32768            # sizes are in KB, not bytes
  fill_policy: RING_BUFFER  # keep newest data; DISCARD keeps oldest
}
duration_ms: 10000          # omit to trace until stopped manually

data_sources {
  config {
    name: "linux.ftrace"    # which producer to enable
    ftrace_config { ... }   # that producer's own options
  }
}
# ...more data_sources blocks, one per source...
```

Merging two configs = keep one `buffers` section and concatenate their
`data_sources` blocks. Each data source may appear at most once (the
`linux.ftrace` config especially: merge all `ftrace_events` /
`atrace_categories` / `atrace_apps` entries into a single block).

## Exemplar configs

| File (under `$SKILL_ROOT/infra-references/example-configs/`) | Use case |
| --- | --- |
| `sched_cpu.pftxt` | CPU scheduling + frequency/idle. The base layer for most investigations. |
| `app_jank.pftxt` | Slow UI / dropped frames: atrace categories, app events, frame timeline. |
| `memory_counters.pftxt` | System + per-process memory over time, LMK activity. |
| `java_heap_dump.pftxt` | Java heap retention graph for one app. |
| `native_heap.pftxt` | Sampled native malloc/free callstacks (heapprofd). |
| `cpu_profile.pftxt` | Periodic CPU callstack samples (traced_perf). |
| `long_background.pftxt` | Long/field traces: ring buffer + periodic file writes. |

For a standalone heap dump / native heap profile / CPU profile, prefer the
dedicated helper scripts in
[recording_android_traces.md]($SKILL_ROOT/infra-references/recording_android_traces.md);
use the exemplars when one trace must combine several sources.

## Data sources at a glance

| `name` | What it records | Key options |
| --- | --- | --- |
| `linux.ftrace` | Kernel events and atrace | `ftrace_events` (e.g. `sched/sched_switch`, `power/cpu_frequency`, `kmem/rss_stat`, `binder/binder_transaction`); `atrace_categories` (`gfx`, `view`, `wm`, `am`, `input`, `binder_driver`, `dalvik`, ...); `atrace_apps` (package name, required for an app's own trace events) |
| `linux.process_stats` | Process/thread names; per-process counters | `scan_all_processes_on_start: true`; `proc_stats_poll_ms` for periodic RSS/OOM-score polling |
| `linux.sys_stats` | /proc counters polled periodically | `meminfo_period_ms` + `meminfo_counters` (`MEMINFO_MEM_FREE`, ...); `vmstat_period_ms` + `vmstat_counters`; `stat_period_ms` + `stat_counters` (CPU times); `cpufreq_period_ms` |
| `android.log` | Logcat | `android_log_config { log_ids: LID_DEFAULT ... }` |
| `android.surfaceflinger.frametimeline` | Per-frame expected/actual timelines (jank classification) | none needed |
| `android.java_hprof` | Java heap dump | `java_hprof_config { process_cmdline: "<pkg>" }` |
| `android.heapprofd` | Native heap profiling | `heapprofd_config { sampling_interval_bytes, process_cmdline, all_heaps }` |
| `linux.perf` | CPU callstack sampling | `perf_event_config { timebase { frequency }, callstack_sampling { scope { target_cmdline } } }` |
| `android.packages_list` | Package name/version/uid mapping | none needed |
| `android.power` | Battery counters | `android_power_config { battery_poll_ms, battery_counters }` |
| `track_event` | SDK-instrumented app events | `track_event_config { enabled_categories }` |

The full, authoritative field list for every data source is the generated
[TraceConfig reference](https://perfetto.dev/docs/reference/trace-config-proto);
per-source guides live under
[perfetto.dev/docs/data-sources](https://perfetto.dev/docs/data-sources/).

## Top-level knobs

- `duration_ms` — trace length. Omit it to trace until the recording
  command is stopped (Ctrl-C on `record_android_trace`).
- `buffers.fill_policy` — `RING_BUFFER` keeps the newest data (right choice
  when the interesting moment is at the end); `DISCARD` keeps the oldest.
- Long traces: `write_into_file: true` + `file_write_period_ms` stream the
  buffer to disk periodically so the trace can exceed RAM;
  `flush_period_ms: 30000` keeps app-emitted events ordered;
  `max_file_size_bytes` bounds the output.
- Buffer sizing rule of thumb: 32–64 MB (`size_kb: 32768`–`65536`) is
  plenty for most 10–30s traces; heap dumps need ~100 MB or
  `write_into_file`.

## Pitfalls

- All buffer sizes are **KB** (`size_kb: 32768` = 32 MB); durations are
  **ms**.
- atrace data (categories and app events) only flows through the
  `linux.ftrace` data source — there is no separate "atrace" source, and
  an app's custom trace events appear only if its package is listed in
  `atrace_apps` (or `atrace_apps: "*"`).
- Field-name typos are only caught when the config is parsed at record
  time: `record_android_trace -c config.pftxt` fails fast with a parse
  error naming the bad field, so treat that as your validator and fix and
  retry.
- Text-format enum values are bare identifiers (`fill_policy: RING_BUFFER`),
  strings are quoted.
