# Recording Perfetto Traces on Linux

> [!IMPORTANT] **Scope:** This guide is for recording traces on **Linux**
> (desktop, server, embedded). For Android devices, read
> [recording_android_traces.md]($SKILL_ROOT/infra-references/recording_android_traces.md)
> instead. For other platforms (macOS, Chrome), see
> [perfetto.dev/docs](https://perfetto.dev/docs/).

On Linux everything is driven by a single statically-linked binary,
`tracebox`, which bundles the tracing daemons (`traced`, `traced_probes`)
and the `perfetto` recording command. There are no helper scripts and no
category shorthand — recording is always config-driven.

## 1. Download tracebox

```bash
curl -LO https://get.perfetto.dev/tracebox
chmod +x tracebox
```

The binary is self-contained and can be copied to any Linux machine
(x86_64 and arm64 builds are served automatically for the current host).

## 2. Write a trace config

Configs are the standard `TraceConfig` protobuf text format — the same
format as everywhere else in Perfetto. Read
[trace_config_reference.md]($SKILL_ROOT/infra-references/trace_config_reference.md)
for the config shape and start from the exemplars in
`$SKILL_ROOT/infra-references/example-configs/`.

Linux-specific notes on the exemplars:

*   `sched_cpu.pftxt`, `memory_counters.pftxt`, `cpu_profile.pftxt` and
    `long_background.pftxt` work as-is on Linux.
*   Anything Android-only does not apply: `atrace_categories` /
    `atrace_apps` inside `ftrace_config`, and the `android.*` data sources
    (`android.java_hprof`, `android.heapprofd`,
    `android.surfaceflinger.frametimeline`, `android.log`, ...). Strip
    those blocks when adapting an exemplar.
*   In `linux.perf` / `heapprofd`-style scopes, `process_cmdline` matches
    the process command line as on Android.

## 3. Record

```bash
# ftrace needs root on most distros: run under sudo.
sudo ./tracebox -o trace.perfetto-trace --txt -c config.pftxt
```

*   `--txt` says the config is text format (omit it for a binary-encoded
    config).
*   If the config has no `duration_ms`, recording runs until Ctrl-C.
*   A config typo fails fast here with a parse error naming the bad
    field — fix the config and retry.
*   Data sources that only poll /proc (`linux.sys_stats`,
    `linux.process_stats`) work without root; ftrace and `linux.perf`
    generally need root or the right capabilities/sysctls
    (`kernel.perf_event_paranoid` for `linux.perf`).

## 4. Analyze

The output file is a normal Perfetto trace: open it in
[ui.perfetto.dev](https://ui.perfetto.dev) or query it directly on the
same machine with `trace_processor` (see
[querying.md]($SKILL_ROOT/infra-references/querying.md)).
