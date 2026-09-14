# Data sources

This page lists every data source you can name in a
[TraceConfig](/docs/reference/trace-config-proto.autogen#TraceConfig), grouped
by what it records. It also lists the process that provides each source and the
platforms it runs on.

NOTE: Platform-side Android data sources (SurfaceFlinger, system_server) and
third-party producers are registered outside this repository. Their names are
listed in
[DataSourceConfig](/docs/reference/trace-config-proto.autogen#DataSourceConfig).
GPU producers may register vendor-suffixed names, e.g. `gpu.counters.adreno`.

## System

| Data source | What it records | Producer | Platforms | Page |
|---|---|---|---|---|
| `linux.ftrace` | Kernel tracepoints: scheduling, CPU frequency and idle, syscalls, kernel function graph, and atrace userspace events | traced_probes | Android 9+, Linux | [CPU scheduling](/docs/data-sources/cpu-scheduling.md), [CPU frequency and idle states](/docs/data-sources/cpu-freq.md), [System calls](/docs/data-sources/syscalls.md), [Kernel function graph](/docs/data-sources/funcgraph.md), [ATrace](/docs/data-sources/atrace.md) |
| `linux.frozen_ftrace` | Ftrace data left in a persistent ring buffer by the previous boot | traced_probes | Linux-based systems with a persistent ring buffer | [Capture ftrace data across a reboot](/docs/data-sources/previous-boot-trace.md) |
| `linux.process_stats` | Process and thread names and hierarchy; polled per-process counters | traced_probes | Android, Linux | [Memory counters and events](/docs/data-sources/memory-counters.md) |
| `linux.sys_stats` | Polled `/proc/meminfo`, `/proc/vmstat`, `/proc/stat`, CPU frequency, disk, PSI and thermal counters | traced_probes | Android, Linux | [Memory counters and events](/docs/data-sources/memory-counters.md) |
| `linux.system_info` | One-off CPU description (model, capacity, available frequencies) and IRQ names | traced_probes | Android, Linux | None |
| `linux.perf` | Sampled callstacks and perf counters | traced_perf | Android, Linux | [Record CPU profiles and perf counters](/docs/getting-started/cpu-profiling.md) |
| `linux.sysfs_power` | Battery counters read from sysfs | traced_probes | Linux, ChromeOS | [Battery and power rails](/docs/data-sources/battery-counters.md) |
| `linux.systemd_journald` | systemd journal log entries | traced_probes | Linux | None |
| `linux.inode_file_map` | Inode to file path mapping (deprecated) | traced_probes | Android, Linux | None |
| `perfetto.metatrace` | Internal self-tracing of the Perfetto daemons | traced, traced_probes, traced_perf | Android, Linux | None |
| `gpu.counters` | GPU performance counters | GPU driver | Android, Linux | [GPU](/docs/data-sources/gpu.md) |
| `gpu.renderstages` | GPU render stage and compute activity timeline | GPU driver | Android, Linux | [GPU](/docs/data-sources/gpu.md) |
| `windows.etw` | Event Tracing for Windows events | ETW-based producer | Windows | None |
| `qnx.kernel` | QNX kernel trace events | trace_qnx_probes | QNX | None |

## Memory

| Data source | What it records | Producer | Platforms | Page |
|---|---|---|---|---|
| `android.heapprofd` | Sampled native heap allocations with callstacks | heapprofd | Android 10+, Linux | [Native heap profiler (heapprofd)](/docs/data-sources/native-heap-profiler.md) |
| `android.java_hprof` | Java/Kotlin heap dumps from ART | heapprofd | Android 11+ | [ART heap dumps](/docs/data-sources/java-heap-profiler.md) |
| `android.java_hprof.oom` | Java heap dump of a process hitting OutOfMemoryError | ART in the app process, armed by traced | Android 14+ | [ART heap dumps](/docs/data-sources/java-heap-profiler.md) |
| `linux.ftrace` | Memory events: `kmem/rss_stat`, `mm_event`, low-memory kills | traced_probes | Android, Linux | [Memory counters and events](/docs/data-sources/memory-counters.md) |
| `vulkan.memory_tracker` | Vulkan memory allocation and bind events | GPU driver | Android, Linux | [GPU](/docs/data-sources/gpu.md) |

## Android

| Data source | What it records | Producer | Platforms | Page |
|---|---|---|---|---|
| `android.log` | logcat messages, text and binary (EventLog) | traced_probes | Android | [Logcat](/docs/data-sources/android-log.md) |
| `android.power` | Battery counters and power rail energy | traced_probes | Android 10+ | [Battery and power rails](/docs/data-sources/battery-counters.md) |
| `android.packages_list` | Installed packages: name, uid, version code, debuggable | traced_probes | Android | None |
| `android.system_property` | Values of `debug.tracing.*` system properties | traced_probes | Android | None |
| `android.polled_state` | Display state (legacy; prefer `android.system_property`) | traced_probes | Android | None |
| `android.user_list` | Users on the device and their types | traced_probes | Android | None |
| `android.aflags` | Snapshots of aconfig feature flag values | traced_probes | Android | [Aflags](/docs/data-sources/android-aflags.md) |
| `android.game_interventions` | Game modes and interventions per game | traced_probes | Android | [Game interventions](/docs/data-sources/android-game-intervention-list.md) |
| `android.kernel_wakelocks` | Polled kernel and native wakelock stats | traced_probes | Android | None |
| `android.cpu_per_uid` | Polled CPU time per UID | traced_probes | Android | None |
| `android.statsd` | statsd atoms | traced_probes | Android (in-tree builds) | None |
| `android.sdk_sysprop_guard` | Switches Skia (HWUI, SurfaceFlinger) from atrace to `track_event` | traced | Android 14 QPR1+ | None |
| `android.surfaceflinger.frametimeline` | Expected and actual frame timelines, jank classification | SurfaceFlinger | Android 12+ | [FrameTimeline](/docs/data-sources/frametimeline.md) |
| `android.surfaceflinger.layers` | SurfaceFlinger layer snapshots | SurfaceFlinger | Android | None |
| `android.surfaceflinger.transactions` | SurfaceFlinger transactions | SurfaceFlinger | Android | None |
| `android.display.video` | Encoded video of each display's contents | Android platform | Android | [Screen recording](/docs/data-sources/video-frames.md) |
| `android.windowmanager` | WindowManager state snapshots | system_server | Android | None |
| `android.inputmethod` | Input method client, service and manager state | Android platform | Android | None |
| `android.protolog` | ProtoLog messages | Android platform | Android | None |
| `android.input.inputevent` | Input events dispatched by the system | Android platform | Android (userdebug/eng) | None |
| `android.network_packets` | Per-packet or aggregated network traffic details | Android platform | Android 14+ | None |
| `android.app_wakelocks` | Wakelocks held by apps | Android platform | Android | None |
| `android.process_state` | Process state snapshots | Android platform | Android | None |
| `android.pixel.modem` | Modem events | Pixel modem | Android (Pixel) | None |

## Apps and SDK

| Data source | What it records | Producer | Platforms | Page |
|---|---|---|---|---|
| `track_event` | Slices, instants, counters and flows emitted with the Perfetto SDK | Apps using the Perfetto SDK | Android, Linux, macOS, Windows | [Track events (C++ SDK)](/docs/instrumentation/track-events.md) |
| `org.chromium.system_metrics` | Chrome system metrics | Chrome | Chrome platforms | None |
| `org.chromium.histogram_samples` | Chrome UMA histogram samples | Chrome | Chrome platforms | None |
| `org.chromium.sampler_profiler` | Chrome stack sampling profiles | Chrome | Chrome platforms | None |
| `org.chromium.native_heap_profiler` | Chrome sampling heap profiles | Chrome | Chrome platforms | None |
| `code.v8.dev` | V8 JavaScript engine events | V8 | Platforms embedding V8 | None |
