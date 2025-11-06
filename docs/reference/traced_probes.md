# TRACED_PROBES(8)

## NAME

traced_probes - System & OS Probes

## DESCRIPTION

`traced_probes` is a specialized daemon that acts as a privileged
[Producer](/docs/concepts/service-model.md#producer) in the Perfetto
architecture. While any application can act as a producer to contribute its own
trace data, `traced_probes` is specifically responsible for collecting
system-level and kernel-level data that typically requires elevated privileges.

## Relationship with `traced`

`traced_probes` is a client of the [`traced`](/docs/reference/traced.md)
service. It connects to `traced`'s producer socket and registers a set of data
sources. `traced` then sends requests to `traced_probes` to start or stop these
data sources as part of a tracing session.

This separation of concerns is a key part of Perfetto's design. `traced` is the
central manager, while `traced_probes` is a specialized data provider. This
decoupled architecture allows for multiple, independent producers and consumers
to interact with the tracing system simultaneously without interfering with each
other.

![traced_probes and traced](/docs/images/platform-tracing.png)

## Data Sources

`traced_probes` provides a wide range of data sources, collecting system-level
and kernel-level data. The configuration for these data sources is specified in
the `DataSourceConfig` section of the overall trace configuration.

Below is a detailed list of the main data sources provided by `traced_probes`,
including their functionality and relevant configuration aspects:

### `linux.ftrace` (Kernel Tracing)

*   **Description**: This is the primary data source for high-frequency kernel
    events. It enables and reads raw ftrace data from the Linux kernel's ftrace
    interface, providing insights into process scheduling, system calls,
    interrupts, and other kernel activities.
*   **Implementation Details**:
    *   **`FtraceDataSource`**: Manages the ftrace session, registers with
        `FtraceController`, and handles flush requests.
    *   **`CpuReader`**: The core component for reading and parsing raw ftrace
        data from per-CPU trace pipes
        (`/sys/kernel/tracing/per_cpu/cpuX/trace_pipe_raw`).
        *   **Batch Reading**: Reads ftrace data in batches of pages for
            efficiency.
        *   **Event Parsing**: Parses ftrace page headers (timestamp, size, lost
            events) and event payloads. It translates binary ftrace events into
            Perfetto protobufs.
        *   **Event Types**: Supports various event types including
            `sched_switch`, `sched_waking` (with compact encoding if enabled),
            `sys_enter`, `sys_exit`, `kprobe` events, and generic ftrace events.
        *   **Symbolization**: Can symbolize kernel addresses using
            `LazyKernelSymbolizer` (if `symbolize_ksyms` is enabled in
            `FtraceConfig`).
        *   **Error Handling**: Designed to handle potentially malicious or
            buggy ftrace data by validating packets and recording
            `FtraceParseStatus` errors in the trace without crashing.
        *   **Filtering**: Supports filtering of `ftrace/print` events based on
            their content.
        *   **Clock Synchronization**: Captures ftrace clock snapshots for
            accurate time synchronization.
*   **Configuration**: Configured via `FtraceConfig` within the
    `DataSourceConfig`. Key options include:
    *   `ftrace_events`: List of ftrace events to enable (e.g.,
        `sched/sched_switch`).
    *   `atrace_categories`, `atrace_apps`: For Android, enables userspace
        Atrace categories and apps.
    *   `syscall_events`: Specific syscalls to trace.
    *   `enable_function_graph`: Enables kernel function graph tracing.
    *   `compact_sched`: Enables compact encoding for scheduler events.
    *   `symbolize_ksyms`: Enables kernel symbolization.
    *   `print_filter`: Filters `ftrace/print` events based on content.

### `linux.process_stats` (Process and Thread Statistics)

*   **Description**: Collects detailed process and thread-level statistics from
    the `/proc` filesystem. It provides both a snapshot of the process tree and
    periodic memory/CPU counters.
*   **Implementation Details**:
    *   **Process Tree (`ProcessTree`)**: Scans `/proc` for numeric directories
        (PIDs). For each PID, it reads `/proc/[pid]/status` and
        `/proc/[pid]/cmdline` to collect process information (PID, PPID, UID,
        command line, kthread status, namespaced PIDs). It also iterates
        `/proc/[pid]/task` to find threads.
    *   **Process Stats (`ProcessStats`)**: Periodically polls various `/proc`
        files:
        *   `/proc/[pid]/status`: Memory counters (VmSize, VmRSS, RssAnon,
            VmSwap, VmLck, VmHWM).
        *   `/proc/[pid]/stat`: Process runtimes (utime, stime).
        *   `/proc/[pid]/smaps_rollup`: Aggregated memory statistics (if
            `scan_smaps_rollup` is enabled).
        *   `/proc/[pid]/oom_score_adj`: Out-of-memory score adjustment.
        *   `/proc/[pid]/dmabuf_rss`: DMA buffer Resident Set Size (if
            `record_process_dmabuf_rss` is enabled).
    *   **File Descriptors**: If `resolve_process_fds` is enabled, it reads
        `/proc/[pid]/fd` and resolves symlinks to get file paths for open file
        descriptors.
*   **Configuration**: Configured via `ProcessStatsConfig`. Key options include:
    *   `record_thread_names`: Record thread names.
    *   `scan_all_processes_on_start`: Dump the entire process tree at the
        start.
    *   `resolve_process_fds`: Resolve file descriptor paths.
    *   `scan_smaps_rollup`: Read `/proc/[pid]/smaps_rollup`.
    *   `record_process_age`: Record process start time.
    *   `record_process_runtime`: Record user and kernel mode CPU times.
    *   `record_process_dmabuf_rss`: Record DMA buffer RSS.
    *   `proc_stats_poll_ms`: Polling interval for periodic stats.
    *   `proc_stats_cache_ttl_ms`: Time-to-live for cached stats.
    *   `quirks`: Special behaviors (e.g., `DISABLE_ON_DEMAND`).

### `linux.sys_stats` (System-Wide Statistics)

*   **Description**: Collects system-wide statistics by periodically polling
    various files in `/proc` and `/sys`.
*   **Implementation Details**:
    *   Reads from `/proc/meminfo`, `/proc/vmstat`, `/proc/stat`,
        `/proc/buddyinfo`, `/proc/diskstats`, `/proc/pressure/*`,
        `/sys/class/thermal/*`, `/sys/devices/system/cpu/*`.
    *   **Meminfo**: Various memory statistics (e.g., MemTotal, MemFree, Cached,
        SwapTotal).
    *   **Vmstat**: Virtual memory statistics (e.g., nr_free_pages, pgpgin,
        pgpgout, pgfault).
    *   **Stat**: Per-CPU times (user, system, idle, iowait, irq, softirq,
        steal), total IRQ counts, softirq counts, and number of forks since
        boot.
    *   **Devfreq**: Device frequencies from `/sys/class/devfreq/*/cur_freq`.
    *   **Cpufreq**: Current CPU frequencies from
        `/sys/devices/system/cpu/cpuX/cpufreq/scaling_cur_freq`.
    *   **Buddyinfo**: Memory fragmentation details from `/proc/buddyinfo`.
    *   **Diskstat**: Disk I/O statistics (read/write sectors, time, discard,
        flush) from `/proc/diskstats`.
    *   **PSI (Pressure Stall Information)**: Total stall time for some/full
        pressure from `/proc/pressure/cpu`, `/proc/pressure/io`,
        `/proc/pressure/memory`.
    *   **Thermal Zones**: Thermal sensor data (temperature, type) from
        `/sys/class/thermal/thermal_zoneX/temp` and
        `/sys/class/thermal/thermal_zoneX/type`.
    *   **CPU Idle States**: CPU idle state durations from
        `/sys/devices/system/cpu/cpuX/cpuidle/stateY/name` and
        `/sys/devices/system/cpu/cpuX/cpuidle/stateY/time`.
    *   **GPU Frequency**: Current GPU frequencies from various sysfs paths
        depending on the GPU vendor (Adreno, Intel, AMD).
*   **Configuration**: Configured via `SysStatsConfig`. Allows fine-grained
    control over which counters to collect and their polling frequencies (e.g.,
    `meminfo_period_ms`, `vmstat_period_ms`, `stat_counters`).

### `linux.power` / `android.power` (Power and Battery Information)

*   **Description**: Collects power and battery statistics. `linux.power` uses
    the Linux sysfs interface, while `android.power` uses Android-specific HALs.
*   **Implementation Details**:
    *   **`LinuxPowerSysfsDataSource`**: Reads from `/sys/class/power_supply` to
        get battery charge, capacity, current, voltage, and energy counters.
    *   **`AndroidPowerDataSource`**: Uses Android's Health HAL to retrieve
        battery counters (charge, capacity, current, voltage), power rail energy
        data, energy consumer information, and power entity state residency.
*   **Configuration**: `AndroidPowerConfig` allows enabling specific battery
    counters (`battery_counters`), power rails (`collect_power_rails`), energy
    estimation breakdown (`collect_energy_estimation_breakdown`), and entity
    state residency (`collect_entity_state_residency`).

### `android.log` (Android Logcat)

*   **Description**: Streams log messages from Android's logcat buffer into the
    trace.
*   **Implementation Details**: Connects to `/dev/socket/logdr` to read log
    entries. It parses both binary (event log) and text (main, system, crash
    logs) formats.
*   **Configuration**: `AndroidLogConfig` allows filtering by log IDs
    (`log_ids`) and tags (`filter_tags`), and setting a minimum priority
    (`min_prio`).

### `android.system_property` (Android System Properties)

*   **Description**: Collects the state of Android system properties.
*   **Implementation Details**: Reads system properties using
    `base::GetAndroidProp()`. Only properties prefixed with `debug.tracing.` are
    allowed for security reasons.
*   **Configuration**: `AndroidSystemPropertyConfig` allows specifying
    `property_name`s to monitor and a `poll_ms` interval.

### `android.packages_list` (Android Package Information)

*   **Description**: Dumps information about installed packages on Android.
*   **Implementation Details**: Reads `/data/system/packages.list` to extract
    package name, UID, debuggable status, profileable status, and version code.
*   **Configuration**: `PackagesListConfig` allows filtering by
    `package_name_filter` and can be configured to
    `only_write_on_cpu_use_every_ms` (polling mode) or dump all at start.

### `android.game_intervention_list` (Android Game Intervention List)

*   **Description**: Dumps the game intervention list from the package manager
    on Android.
*   **Implementation Details**: Reads `/data/system/game_mode_intervention.list`
    to get information about game packages, their current game mode, and
    interventions (e.g., ANGLE usage, resolution downscale, FPS limits).
*   **Configuration**: `AndroidGameInterventionListConfig` allows filtering by
    `package_name_filter`.

### `android.cpu.uid` (Per-UID CPU Time)

*   **Description**: Collects per-UID CPU time from the kernel.
*   **Implementation Details**: Uses `AndroidCpuPerUidPoller` to get CPU times
    grouped by UID and CPU cluster. It calculates deltas between polls to report
    incremental CPU usage.
*   **Configuration**: `CpuPerUidConfig` allows setting the `poll_ms` interval.

### `android.kernel_wakelocks` (Kernel Wakelocks)

*   **Description**: Collects kernel wakelock information.
*   **Implementation Details**: Dynamically loads
    `libperfetto_android_internal.so` to access kernel wakelock data. It tracks
    wakelock IDs and reports incremental time held.
*   **Configuration**: `KernelWakelocksConfig` allows setting the `poll_ms`
    interval.

### `inode_file_map` (Inode to File Path Mapping)

*   **Description**: Maps inode numbers to file paths, which is useful for
    correlating I/O events with the files being accessed.
*   **Implementation Details**:
    *   **`FileScanner`**: Scans specified root directories (e.g., `/system`) to
        build a static map of inodes to file paths.
    *   **`LRUInodeCache`**: Maintains a cache of recently seen inodes and their
        paths.
    *   Responds to `OnInodes()` calls (typically from `FtraceDataSource`) to
        resolve inode numbers to paths. If an inode is not found in the static
        map or cache, it initiates a dynamic filesystem scan.
*   **Configuration**: `InodeFileConfig` allows specifying `scan_mount_points`,
    `mount_point_mapping` (to remap scan roots), `scan_interval_ms`,
    `scan_delay_ms`, `scan_batch_size`, and `do_not_scan`.

### `metatrace` (Perfetto Self-Tracing)

*   **Description**: A self-tracing data source that records events within
    Perfetto itself, useful for debugging and performance analysis of the
    tracing system.
*   **Implementation Details**: Uses `MetatraceWriter` to hook into Perfetto's
    internal metatrace ring buffer and write events to the trace.
*   **Configuration**: No specific configuration in `DataSourceConfig`.

### `system_info` (System Information)

*   **Description**: Records general information about the system, such as CPU
    details and kernel version.
*   **Implementation Details**: Reads `/proc/cpuinfo` to extract CPU processor
    name, architecture, implementer, variant, part, revision, and feature flags.
    It also reads CPU capacity from `/sys/devices/system/cpu/cpuX/cpu_capacity`
    and CPU frequencies using `CpuFreqInfo`.
*   **Configuration**: No specific configuration in `DataSourceConfig`.

### `initial_display_state` (Android Initial Display State)

*   **Description**: Records the initial display state (e.g., screen on/off,
    brightness) on Android.
*   **Implementation Details**: Reads Android system properties like
    `debug.tracing.screen_state` and `debug.tracing.screen_brightness`.
*   **Configuration**: `AndroidPolledStateConfig` allows setting a `poll_ms`
    interval.

### `statsd_binder` (Android StatsD Atoms)

*   **Description**: Collects StatsD atoms from the binder interface on Android.
*   **Implementation Details**: Connects to the Android StatsD service via
    Binder to subscribe to push and pull atoms. It uses
    `CreateStatsdShellConfig()` to generate the subscription configuration.
*   **Configuration**: `StatsdTracingConfig` allows specifying `pull_config`
    (for pull atoms with frequency and packages) and `push_atom_id` (for push
    atoms).

## Security and Privileges

`traced_probes` often needs to run with elevated privileges (e.g., `root` or
`system` user on Android) to access kernel interfaces like `debugfs` or `/proc`.
Separating these high-privilege probes into their own daemon is a key part of
Perfetto's security model. It ensures that only a minimal amount of code runs
with high privileges, adhering to the principle of least privilege.

## Configuration

The data sources provided by `traced_probes` are configured within the main
trace configuration protobuf that is sent to `traced`. For example, to enable
ftrace, you would include an `FtraceConfig` within the `DataSourceConfig` for
the `linux.ftrace` data source.
