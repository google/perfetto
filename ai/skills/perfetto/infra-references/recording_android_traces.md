# Recording Perfetto Traces on Android (Helper Scripts)

> [!IMPORTANT] **Scope:** This guide is **strictly for recording traces on
> Android devices**. For other platforms (such as Linux, macOS, or Chrome),
> please refer to the platform-specific documentation on
> [perfetto.dev/docs](https://perfetto.dev/docs/).

Rather than running raw `adb` commands, you should use the official Perfetto
helper scripts. They automatically handle pushing configurations, starting the
tracing daemons, pulling the trace file, and optionally opening it in the
browser.

Ensure **Developer options** and **USB debugging** are enabled, and your device
is connected via USB before starting.

--------------------------------------------------------------------------------

## 0. Download the Helper Scripts

Since these tools need to work across different environments, download them
directly from the official Perfetto repository:

```bash
# Java Heap Dump (ART)
curl -O https://raw.githubusercontent.com/google/perfetto/main/tools/java_heap_dump
chmod +x java_heap_dump

# Native Heap Profiling (heapprofd)
curl -O https://raw.githubusercontent.com/google/perfetto/main/tools/heap_profile
chmod +x heap_profile

# CPU Stack Sampling (traced_perf)
curl -O https://raw.githubusercontent.com/google/perfetto/main/tools/cpu_profile
chmod +x cpu_profile

# General Tracing (Ftrace, ATrace, custom configs)
curl -O https://raw.githubusercontent.com/google/perfetto/main/tools/record_android_trace
chmod +x record_android_trace
```

--------------------------------------------------------------------------------

## 1. Memory Tracing

Use these tools to analyze memory leaks, object retention, C/C++ allocations, or
system-wide memory counters.

### A. Java Heap Dump (ART)

Capture a snapshot of all Java objects in a process to investigate memory leaks.

*   *Data Source Docs:*
    [Java Heap Profiler](https://perfetto.dev/docs/data-sources/java-heap-profiler)

```bash
# Trigger a Java heap dump for a specific app
./java_heap_dump -n YOUR_APP_PACKAGE_NAME -o ./heap_dump.perfetto-trace
```

### B. Native C/C++ Heap Profiling (heapprofd)

Track C/C++ memory allocations (malloc/free) to find native leaks.

*   *Data Source Docs:*
    [Native Heap Profiler](https://perfetto.dev/docs/data-sources/native-heap-profiler)

```bash
# Profile native allocations for a specific app
./heap_profile -n YOUR_APP_PACKAGE_NAME

# Profile with custom sampling interval (default is 4096 bytes)
./heap_profile -n YOUR_APP_PACKAGE_NAME -i 2048
```

### C. System-wide Memory Counters

Track RSS, Swap, and process stats over time.

*   *Data Source Docs:*
    [Memory Counters](https://perfetto.dev/docs/data-sources/memory-counters-sys-stats)

*Note: To record memory counters, you must use a custom config via
`record_android_trace` (see Section 4).*

--------------------------------------------------------------------------------

## 2. Stack Sampling / Callstack Profiling (traced_perf)

Identify CPU hot spots in C/C++ or Rust code by periodically sampling
callstacks.

*   *Data Source Docs:*
    [CPU Profiler (traced_perf)](https://perfetto.dev/docs/data-sources/cpu-profiler)

```bash
# Profile CPU usage by sampling callstacks at 100Hz (default) for 10 seconds
./cpu_profile -n YOUR_APP_PACKAGE_NAME -d 10000

# Profile at a custom frequency (e.g., 200Hz)
./cpu_profile -n YOUR_APP_PACKAGE_NAME -f 200
```

--------------------------------------------------------------------------------

## 3. System Tracing (CPU, Scheduling, & ATrace)

Investigate jank, slow transitions, CPU scheduling, and system calls.

*   *Data Source Docs:*
    [Ftrace & ATrace](https://perfetto.dev/docs/data-sources/ftrace)

You can specify duration, buffer size, and categories directly on the command
line:

```bash
# Record scheduling, frequency, and window manager events for 5 seconds
./record_android_trace -t 5s -b 32mb sched gfx wm -a YOUR_APP_PACKAGE_NAME
```

Common categories: `sched` (CPU scheduling), `freq` (CPU frequency), `gfx`
(Graphics), `am` (Activity Manager), `wm` (Window Manager), `view` (View
System).

--------------------------------------------------------------------------------

## 4. Fallback: "Choose Your Own Adventure" (Custom Configs)

If you need a custom mixture of data sources (e.g., combining Java heap dumps
with ftrace) that is not covered by the specialized scripts:

1.  **Synthesize the Config:** Draft a custom Perfetto protobuf text
    configuration (`config.pftxt`) based on the data source schemas and examples
    from the official
    [Perfetto Data Sources Documentation](https://perfetto.dev/docs/data-sources/).
2.  **Save the Config:** Write the synthesized text configuration to a local
    file (e.g., `config.pftxt`).
3.  **Execute the Trace:** Run the trace using the general recorder script
    `record_android_trace` (the other specialized scripts will not work for
    custom configs):

    ```bash
    ./record_android_trace -c config.pftxt -o ./my_trace.perfetto-trace
    ```
