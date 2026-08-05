# Kernel function graph tracing

The Linux kernel's `function_graph` tracer records **every entry into and exit
from kernel functions**, letting you see the exact call tree the kernel executed
on a CPU, with per-function durations. Perfetto can drive this tracer through the
`linux.ftrace` data source and visualises the resulting calls as nested slices
on the timeline, just like userspace slices.

This is a powerful way to answer "what was the kernel actually doing here?"
without adding any instrumentation of your own. It is, however, a high-bandwidth
feature: tracing too many functions will overwhelm the trace buffer, so it is
designed to be used together with filters.

If instead you want to add your **own** tracepoints to the kernel, see
[Instrumenting the kernel with ftrace](/docs/getting-started/ftrace.md).

## Requirements

- A kernel compiled with `CONFIG_FUNCTION_GRAPH_TRACER`. You can confirm support
  with:
  ```bash
  cat /sys/kernel/tracing/available_tracers
  ```
  The output must include `function_graph`.
- `symbolize_ksyms: true` in the ftrace config. Without it, every function shows
  up as a raw hexadecimal address. See
  [Symbolization: kernel symbols](/docs/learning-more/symbolization.md#ftrace)
  for why kernel symbols must be resolved at record time and cannot be added
  afterwards with `trace_processor bundle`.
- On **Android**, function graph tracing is available only on `debuggable`
  (userdebug/eng) builds, and was introduced in Android U.
- `traced_probes` must run as root (or `kptr_restrict` lowered), both to read
  `/proc/kallsyms` and to control the kernel tracer.

## TraceConfig

The relevant options live in `FtraceConfig`:

- `enable_function_graph`: turns on the `function_graph` tracer.
- `function_filters`: a set of globs; only matching functions are traced.
- `function_graph_roots`: a set of globs; matching functions **and all of their
  callees** are traced.
- `function_graph_max_depth`: limit how many call levels below a root are traced.
- `symbolize_ksyms`: required to get function names instead of addresses.

WARNING: Always constrain the traced set with `function_filters` and/or
`function_graph_roots`. Tracing all kernel functions generates an enormous event
stream that will fill the buffer in milliseconds and is rarely useful.

Example: trace the scheduler functions and everything they call, for 10 seconds.

```protobuf
buffers {
  size_kb: 65536
  fill_policy: DISCARD
}

data_sources {
  config {
    name: "linux.ftrace"
    ftrace_config {
      symbolize_ksyms: true
      enable_function_graph: true
      # Trace these functions and all of their callees.
      function_graph_roots: "__schedule"
      # Optionally also keep a flat set of functions of interest.
      function_filters: "handle_mm_fault"
      function_graph_max_depth: 10
    }
  }
}

duration_ms: 10000
```

Record it with `tracebox`:

```bash
./tracebox -c funcgraph.cfg --txt -o funcgraph.pftrace
```

See the [system tracing guide](/docs/getting-started/system-tracing.md) for how
to set up `tracebox` and the necessary permissions on Linux.

## UI

Function graph calls appear as nested slices. Each kernel function entry/exit
pair becomes a slice whose duration is the time spent inside that function
(including callees), so the call tree reads exactly like a userspace flame chart.

- Calls that happen while a thread is running are attached to a **Funcgraph**
  track under that thread.
- Calls that happen on an idle CPU (in the `swapper` idle task) are grouped onto
  a per-CPU `swapper<N> -funcgraph` track.

You can select a slice to see the function name and duration, and use the
flamegraph/aggregation features as with any other slice track.

## SQL

Function graph calls are ordinary slices, so they live in the `slice` table and
can be queried like any other slice. For example, to find the kernel functions
that accounted for the most aggregate time:

```sql
SELECT name, COUNT(*) AS calls, SUM(dur) AS total_dur
FROM slice
JOIN track ON slice.track_id = track.id
WHERE track.name = 'Funcgraph'
GROUP BY name
ORDER BY total_dur DESC
LIMIT 20;
```

## Troubleshooting

- **Functions show as hex addresses** (e.g. `0xffffffff8108abcd`): you did not
  set `symbolize_ksyms: true`, or `traced_probes` could not read
  `/proc/kallsyms` (not root / `kptr_restrict` too high). This must be fixed at
  record time; see
  [Symbolization: kernel symbols](/docs/learning-more/symbolization.md#ftrace).
- **No function graph data at all**: confirm `function_graph` is in
  `available_tracers`, and that your config sets at least one of
  `function_filters` / `function_graph_roots`.
- **The data source was rejected**: function graph cannot run alongside another
  concurrent `linux.ftrace` data source that uses a different kernel tracer, as
  the kernel tracer cannot be switched mid-trace.
