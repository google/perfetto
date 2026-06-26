# Cookbook: Linux Tracing Recipes

This page collects **end-to-end recipes** for profiling and tracing your own
programs on Linux: how to build so that traces can be symbolized, how to record
the most common kinds of trace, and how to turn raw addresses into function
names afterwards.

It is aimed at developers profiling native binaries on a Linux host or embedded
Linux target (including Yocto, QNX and similar). Each recipe is self-contained
and includes the commands to download the tools it needs. For the full reference
on each topic, follow the links into the deeper guides:

- [Recording system traces](/docs/getting-started/system-tracing.md)
- [CPU profiling and perf counters](/docs/getting-started/cpu-profiling.md)
- [Native heap profiling](/docs/data-sources/native-heap-profiler.md)
- [Kernel function graph tracing](/docs/data-sources/funcgraph.md)
- [Symbolization and deobfuscation](/docs/learning-more/symbolization.md)

## Setup: tools and permissions {#setup}

Two tools cover everything on this page. Both are single self-contained
downloads:

- **`tracebox`**: the recording engine. It bundles `traced`, `traced_probes` and
  all data source implementations into one statically linked binary.
  ```bash
  curl -LO https://get.perfetto.dev/tracebox
  chmod +x tracebox
  ```
- **`traceconv`**: a host-side toolkit for converting and, importantly here,
  symbolizing traces. It is a thin Python wrapper that downloads the right native
  binary for your platform on first use.
  ```bash
  curl -LO https://get.perfetto.dev/traceconv
  chmod +x traceconv
  ```

Recording from ftrace and `perf_event_open` needs elevated privileges. The
simplest option is to run `tracebox` as root (`sudo ./tracebox ...`).
Alternatively, grant the specific permissions once per boot:

```bash
# ftrace-based data sources (scheduling, function_graph, ...).
sudo chown -R $USER /sys/kernel/tracing

# perf / callstack sampling (linux.perf).
echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid

# Resolve KERNEL symbol names (kallsyms). Needed for kernel frames in
# callstacks and for the function_graph recipe below.
echo 0  | sudo tee /proc/sys/kernel/kptr_restrict
```

## Building binaries Perfetto can symbolize {#building-with-symbols}

Perfetto records raw instruction **addresses** for native callstacks (from the
CPU profiler and the heap profiler). To turn those addresses into function names,
files and line numbers, the host doing the symbolization needs unstripped ELF
binaries whose **Build ID matches** the ones that ran on the target. Do this
before you record.

**1. Compile with debug info.** Add `-g`. This does not change the generated
code, only the DWARF debug info attached to it:

```bash
gcc -g -O2 -o myapp myapp.c        # or clang, same flags
```

`-O2 -g` is the recommended combination for profiling: optimised code (so you
profile what you actually ship) with enough debug info to map addresses back to
source lines.

**2. Keep a Build ID.** Modern toolchains emit a GNU Build ID by default. Confirm
it with:

```bash
readelf -n ./myapp | grep -A1 'Build ID'
```

The Build ID is how Perfetto matches a binary on disk to the mapping recorded in
the trace. Two different builds have different Build IDs, and Perfetto will
refuse to apply mismatched symbols (this is a feature, it prevents wrong
symbolization).

**3. (Optional) Ship stripped, keep the symbols.** You do not need to deploy
debug info to the target. Split it into a sidecar file and strip the deployed
binary; matching is by Build ID, so the filenames need not line up:

```bash
gcc -g -O2 -o myapp myapp.c

# Split debug info into a sidecar, linked back by Build ID.
objcopy --only-keep-debug myapp myapp.debug
objcopy --strip-debug --add-gnu-debuglink=myapp.debug myapp

# Deploy the small, stripped `myapp` to the target.
# Keep `myapp.debug` (or the original unstripped binary) on your host.
```

On distributions that package debug info separately (`-dbg` / `-dbgsym` /
`debuginfo` packages), installing those on your host gives the same result:
unstripped symbols under `/usr/lib/debug`.

## Recipe: CPU profiling with full symbols {#cpu-profiling}

Goal: a flamegraph of where a process spends CPU time, with real function names.
This is the end-to-end version of the
[CPU profiling guide](/docs/getting-started/cpu-profiling.md).

**1. Build with symbols** as described [above](#building-with-symbols).

**2. Write a config.** This samples callstacks 100 times per second per CPU,
unwinding only when your process is on-CPU, and adds scheduling context. Save it
as `cpu.cfg` and change `target_cmdline` to a substring of your process name:

```protobuf
duration_ms: 10000

buffers {
  size_kb: 65536
  fill_policy: DISCARD
}

# Periodic callstack sampling, scoped to one process.
data_sources {
  config {
    name: "linux.perf"
    perf_event_config {
      timebase {
        counter: SW_CPU_CLOCK
        frequency: 100
        timestamp_clock: PERF_CLOCK_MONOTONIC
      }
      callstack_sampling {
        scope {
          target_cmdline: "myapp"
        }
        # Also unwind into the kernel. Needs kptr_restrict lowered (see Setup).
        kernel_frames: true
      }
    }
  }
}

# Scheduling context on the same timeline.
data_sources {
  config {
    name: "linux.ftrace"
    ftrace_config {
      ftrace_events: "sched/sched_switch"
      ftrace_events: "sched/sched_waking"
    }
  }
}

# Process and thread names.
data_sources {
  config {
    name: "linux.process_stats"
    process_stats_config {
      scan_all_processes_on_start: true
    }
  }
}
```

**3. Record** (see [Setup](#setup) for the `tracebox` download and permissions):

```bash
sudo ./tracebox -c cpu.cfg --txt -o /tmp/trace.pftrace
```

At this point **kernel** frames are already symbolized (resolved on-device from
kallsyms), but **userspace** frames are still raw addresses.

**4. Bake in userspace symbols** with `traceconv bundle`. It auto-discovers the
binaries that were loaded (using the absolute paths recorded in the trace, which
works well when you profiled on the same machine), and writes a single
self-contained trace:

```bash
# llvm-symbolizer must be on $PATH, e.g. `sudo apt install llvm`.
./traceconv bundle /tmp/trace.pftrace /tmp/trace.bundle
```

If your symbols live elsewhere (a build host, a `.debug` directory, an embedded
sysroot), point `bundle` at them:

```bash
./traceconv bundle \
  --symbol-paths /path/to/sysroot/usr/lib/debug,/path/to/build/out \
  --verbose \
  /tmp/trace.pftrace /tmp/trace.bundle
```

**5. View.** Open `/tmp/trace.bundle` in the
[Perfetto UI](https://ui.perfetto.dev); select a time range over the samples to
get a flamegraph. The Build-ID lookup order and "could not find library"
troubleshooting are documented in the
[symbolization guide](/docs/learning-more/symbolization.md#callstacks).

To instead produce aggregated [pprof](https://github.com/google/pprof) profiles:

```bash
./traceconv profile --perf /tmp/trace.pftrace
```

## Recipe: Native heap (memory) profiling {#heap-profiling}

Goal: see which callstacks allocated the most native (malloc) memory. On Linux
the `heap_profile` helper drives this end to end, including launching your binary
with the profiler preloaded.

Download the helper script:

```bash
curl -LO https://raw.githubusercontent.com/google/perfetto/main/tools/heap_profile
chmod +x heap_profile
```

Run your binary under it:

```bash
python3 heap_profile host -- ./myapp --some-flag
```

The script auto-downloads `tracebox` and the `libheapprofd_glibc_preload.so`
preload library, runs your binary with heapprofd attached, and on exit (or
`Ctrl-C`) writes a `raw-trace` plus per-process pprof files to a `/tmp` directory
it prints. Because you profiled locally, the matching binaries are present, so
symbols resolve automatically.

Open the `raw-trace` file in the [Perfetto UI](https://ui.perfetto.dev) to see
the allocation flamegraph. See
[native heap profiler: Linux support](/docs/data-sources/native-heap-profiler.md#non-android-linux-support)
for the full set of options (custom preload libraries, sampling interval, etc.).

## Recipe: Kernel function graph tracing {#funcgraph}

Goal: see exactly which kernel functions ran, and for how long, as nested slices.
The single most common mistake is forgetting `symbolize_ksyms`, which leaves
every function as a hex address.

Save as `funcgraph.cfg` (this traces `__schedule` and everything it calls):

```protobuf
duration_ms: 10000

buffers {
  size_kb: 65536
  fill_policy: DISCARD
}

data_sources {
  config {
    name: "linux.ftrace"
    ftrace_config {
      # Without this, functions show as hex addresses.
      symbolize_ksyms: true
      enable_function_graph: true
      function_graph_roots: "__schedule"
      function_graph_max_depth: 10
    }
  }
}
```

Record it (function graph drives the kernel tracer, so root is required):

```bash
sudo ./tracebox -c funcgraph.cfg --txt -o /tmp/funcgraph.pftrace
```

Open `/tmp/funcgraph.pftrace` in the UI; the calls appear as nested slices on a
per-thread `Funcgraph` track. See the dedicated
[function graph data source](/docs/data-sources/funcgraph.md) page for the kernel
requirements (`CONFIG_FUNCTION_GRAPH_TRACER`), the filtering options, and how the
calls are visualised. Note that, unlike the [CPU profile](#cpu-profiling) recipe,
these kernel symbols come from `symbolize_ksyms` and **cannot** be added later
with `traceconv bundle`.

## Recipe: Finding why a thread is blocked {#blocked-thread}

Goal: understand why a thread keeps getting descheduled (lock contention,
priority inversion, blocking syscalls).

On Linux the right tool is **callstack sampling triggered by scheduling events**:
use a `sched/sched_switch` (and `sched/sched_waking`) tracepoint as the perf
`timebase`, so you capture a callstack at the exact moment a thread blocks or is
woken. This is far more precise than time-based sampling for blockage analysis.

WARNING: The Android `blocked_function` field (from the
`sched/sched_blocked_reason` ftrace event used in the
[Android cookbook](/docs/getting-started/android-trace-analysis.md)) is an
Android kernel feature and is generally **not** present on upstream/desktop Linux
kernels. Use the callstack-sampling approach below instead.

Minimal config (save as `blocked.cfg`, adjust the `comm` filters to your
process). The tracepoint `filter` keeps the sampler from being overrun by
unrelated threads:

```protobuf
duration_ms: 10000

buffers {
  size_kb: 102400
  fill_policy: DISCARD
}

data_sources {
  config {
    name: "linux.perf"
    perf_event_config {
      timebase {
        period: 1
        tracepoint {
          name: "sched/sched_switch"
          filter: "prev_comm ~ \"*myapp*\" || next_comm ~ \"*myapp*\""
        }
        timestamp_clock: PERF_CLOCK_MONOTONIC
      }
      callstack_sampling {
        kernel_frames: true
      }
      ring_buffer_pages: 2048
    }
  }
}
```

Record and symbolize exactly as in the [CPU profiling recipe](#cpu-profiling)
(`sudo ./tracebox -c blocked.cfg --txt -o ...`, then `./traceconv bundle ...`).

For a full worked example, including filtering on both `sched_switch` and
`sched_waking` and how to reason about the captured callstacks, see the
[scheduling blockages case study](/docs/case-studies/scheduling-blockages.md).
