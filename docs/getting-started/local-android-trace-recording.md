# Cookbook: Local Android Trace Recording

This page collects **end-to-end recipes** for recording Perfetto traces on
Android in situations that the standard interactive workflow does not cover.

- [Tracing Android boot](#boot-tracing): record a trace covering the boot
  sequence, which you cannot start by hand while the device is booting.
- [Capturing a heap dump on OutOfMemoryError](#oom-heap-dump): automatically
  dump the Java heap when an app crashes with an `OutOfMemoryError`.

The recipes assume a host with `adb` access to the device. Each recipe is
self-contained: copy the config and commands as they are, then adjust the
highlighted parameters. If you have never recorded a trace before, start with
the [system tracing tutorial](/docs/getting-started/system-tracing.md). For the
full reference on each topic, follow the links into the deeper guides:

- [Trace configuration](/docs/concepts/config.md)
- [ART heap dumps](/docs/data-sources/java-heap-profiler.md)
- [Analysing Android traces](/docs/getting-started/android-trace-analysis.md)

## Recipe: Tracing Android boot {#boot-tracing}

Goal: record a trace covering the Android boot sequence, to profile process
startup, scheduling and everything else that happens while the device boots.

You cannot start a trace by hand while the device is still booting. Instead,
since Android 13 (T), perfetto can be armed to start recording automatically on
the next boot.

**1. Write a config.** The boot trace config must be in **text** format (not
binary). Save the following as `boottrace.pbtxt`. It records process scheduling
and lifecycle events, but any
[trace configuration](/docs/concepts/config.md) works here (more examples in
[/test/configs/](/test/configs/)):

```protobuf
# One buffer allocated within the central tracing binary for the entire trace,
# shared by the two data sources below.
buffers {
  size_kb: 32768
  fill_policy: DISCARD
}

# Ftrace data from the kernel, mainly the process scheduling events.
data_sources {
  config {
    name: "linux.ftrace"
    target_buffer: 0
    ftrace_config {
      ftrace_events: "sched_switch"
      ftrace_events: "sched_waking"
      ftrace_events: "sched_wakeup_new"

      ftrace_events: "task_newtask"
      ftrace_events: "task_rename"

      ftrace_events: "sched_process_exec"
      ftrace_events: "sched_process_exit"
      ftrace_events: "sched_process_fork"
      ftrace_events: "sched_process_free"
      ftrace_events: "sched_process_hang"
      ftrace_events: "sched_process_wait"
    }
  }
}

# Resolve process commandlines and parent/child relationships, to better
# interpret the ftrace events, which are in terms of pids.
data_sources {
  config {
    name: "linux.process_stats"
    target_buffer: 0
  }
}

# 10s trace, but can be stopped prematurely via `adb shell pkill perfetto`.
duration_ms: 10000
```

**2. Push the config to the device.** The path is fixed; perfetto only looks
for `/data/misc/perfetto-configs/boottrace.pbtxt`:

```bash
adb push boottrace.pbtxt /data/misc/perfetto-configs/boottrace.pbtxt
```

**3. Arm tracing for the next boot:**

```bash
adb shell setprop persist.debug.perfetto.boottrace 1
```

The property is reset during boot, so each boot trace is one-shot: to trace
another boot, set the property again.

**4. Reboot the device:**

```bash
adb reboot
```

**5. Pull the trace.** The trace is written to
`/data/misc/perfetto-traces/boottrace.perfetto-trace`. The file appears only
after the recording has stopped, once `duration_ms` has elapsed, so keep it to
a reasonable value. (If your config sets `write_into_file: true`, the file is
instead written incrementally, every `file_write_period_ms`.)

```bash
adb pull /data/misc/perfetto-traces/boottrace.perfetto-trace
```

The file is removed before a new boot trace starts, so pull it before arming
the next one.

**6. View it.** Open `boottrace.perfetto-trace` in the
[Perfetto UI](https://ui.perfetto.dev). To dig into the data with SQL, see the
[Android trace analysis cookbook](/docs/getting-started/android-trace-analysis.md).

### How early in boot does the trace start?

The trace is started by the `perfetto_trace_on_boot` oneshot init service,
defined in [perfetto.rc](/perfetto.rc). Init starts it once three conditions
hold: persistent properties have been loaded (which happens only after `/data`
has been mounted), the `traced` daemon is up, and boot has not completed yet.
The last condition is why setting the property on a booted device arms the
*next* boot instead of starting a trace immediately. The earliest boot stages
(kernel init, mounting filesystems) are therefore not covered by the trace.

## Recipe: Capturing a heap dump on OutOfMemoryError {#oom-heap-dump}

Goal: automatically capture an ART (Java/Kotlin) heap dump at the moment a
process crashes with a `java.lang.OutOfMemoryError`, so you can see exactly
what was keeping memory alive when allocations started failing.

Since Android 14 (U), ART notifies perfetto when a Java process is about to
crash with an `OutOfMemoryError`, and perfetto can use that notification as a
trigger to dump the Java heap of the crashing process.

### Option A: using the helper script

If you have a perfetto checkout, `tools/java_heap_dump` drives this end to end.
Pass `--wait-for-oom` together with the process to watch (`-n '*'` matches all
processes):

```bash
tools/java_heap_dump --wait-for-oom --oom-wait-seconds 3600 \
  -n 'com.example.myapp' -o oome.pftrace
```

The script starts a tracing session, waits up to `--oom-wait-seconds` for an
`OutOfMemoryError` to be thrown, then pulls the heap dump to `oome.pftrace`.

### Option B: using only adb

If you don't have a checkout, the following command does the same with nothing
but `adb` access. It is safe to copy-paste as-is:

```bash
cat << EOF | adb shell perfetto -c - --txt -o /data/misc/perfetto-traces/oome.pftrace
buffers: {
    size_kb: 524288
    fill_policy: DISCARD
}

data_sources: {
    config {
        name: "android.java_hprof.oom"
        java_hprof_config {
          process_cmdline: "*"
        }
    }
}

data_source_stop_timeout_ms: 100000

trigger_config {
    trigger_mode: START_TRACING
    trigger_timeout_ms: 3600000
    triggers {
      name: "com.android.telemetry.art-outofmemory"
      stop_delay_ms: 500
    }
}
data_sources {
  config {
    name: "android.packages_list"
  }
}
EOF
```

This starts a tracing session that waits for up to one hour
(`trigger_timeout_ms`) for any ART runtime instance to hit an
`OutOfMemoryError`. To watch only your own app, replace the `"*"` in
`process_cmdline` with its process name (e.g. `"com.example.myapp"`).

Once an error is hit, the heap is dumped and tracing stops:

```text
[862.335]    perfetto_cmd.cc:1047 Connected to the Perfetto traced service, TTL: 3601s
[871.335]    perfetto_cmd.cc:1210 Wrote 19487866 bytes into /data/misc/perfetto-traces/oome.pftrace
```

Then pull the heap dump:

```bash
adb pull /data/misc/perfetto-traces/oome.pftrace
```

### Analysing the heap dump

Open `oome.pftrace` in the [Perfetto UI](https://ui.perfetto.dev) and click the
diamond marker in the _"Heap Profile"_ track to get a flamegraph of what
retained the memory. For a guided investigation, see:

- [Heap Dump Explorer](/docs/visualization/heap-dump-explorer.md), interactive
  dominator-tree and class-level analysis of heap dumps.
- [Debugging memory usage](/docs/case-studies/memory.md), an end-to-end guide
  to investigating Android memory issues.
- [ART heap dumps](/docs/data-sources/java-heap-profiler.md), the full
  reference for the underlying data source.
