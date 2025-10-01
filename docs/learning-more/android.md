# Advanced System Tracing on Android

This guide dives deeper into recording system traces on Android, building on the
concepts introduced in the
[System Tracing](/docs/getting-started/system-tracing.md) guide.

Before you continue, you should be familiar with the basics of recording a
system trace using either the
[Perfetto UI](/docs/getting-started/system-tracing.md#android-perfetto-ui) or
the
[`record_android_trace`](/docs/getting-started/system-tracing.md#android-command-line)
script.

This guide covers the lower-level details that these tools abstract away,
including:

- Enabling the Perfetto tracing services on older Android versions.
- Using the on-device `/system/bin/perfetto` binary directly.
- Writing and using a full trace config for advanced customization.
- Using exclusive mode for tracing sessions to prevent interference from other traces.

## Prerequisites: Enabling Tracing Services

Perfetto's tracing daemons (`traced`) are built into Android, but they are only
enabled by default on **Android 11 (R) and newer**.

If you are using **Android 9 (P)** or **Android 10 (Q)**, you must first enable
the tracing services by running the following command:

```bash
# Needed only on Android 9 (P) and 10 (Q) on non-Pixel phones.
adb shell setprop persist.traced.enable 1
```

NOTE: If you are using a version of Android older than 9 (P), the on-device
tools will not work. You must use the
[`record_android_trace`](/docs/getting-started/system-tracing.md#android-command-line)
script.

## Recording using the on-device /system/bin/perfetto command

The `record_android_trace` script is a wrapper around the on-device
`/system/bin/perfetto` binary. For most use cases, the script is recommended,
but you can also invoke the binary directly for more control.

```bash
# Example of invoking the on-device binary directly.
adb shell perfetto \
  # The path to the output file on device.
  # Time to record the trace.
  -o /data/misc/perfetto-traces/trace_file.perfetto-trace \
  # Time to record the trace.
  -t 20s \
  # The atrace categories to record.
  sched freq idle am wm gfx view binder_driver hal dalvik input res memory
```

However, there are several caveats to be aware of when using
`adb shell perfetto` directly:

- **Stopping the trace**: `Ctrl+C` does not work reliably with
  `adb shell perfetto`. It is only propagated correctly when using an
  interactive PTY-based session (i.e., running `adb shell` first, then
  `perfetto` inside the shell). For long-running traces, it is safer to use the
  `--background` flag and `kill` the process by its PID. See the
  [Tracing in the Background](/docs/learning-more/tracing-in-background.md)
  guide for more.

- **Passing trace configs**: On non-rooted devices before Android 12, SELinux
  rules prevent the `perfetto` process from reading config files from
  world-writable locations like `/data/local/tmp`. The recommended workaround is
  to pipe the config via standard input:
  `cat config.pbtx | adb shell perfetto -c -`. Since Android 12, you can place
  configs in `/data/misc/perfetto-configs` and pass the path directly.

- **Pulling trace files**: On devices before Android 10, `adb pull` may not be
  able to access the trace file directly due to permissions. The workaround is
  to use `adb shell cat`:
  `adb shell cat /data/misc/perfetto-traces/trace > trace.pftrace`.

## Using a Full Trace Config

For full control over the tracing process, you can provide a complete trace
config file instead of using command-line flags. This allows you to enable
multiple data sources and fine-tune their settings.

See the [Trace Configuration](/docs/concepts/config.md) page for a detailed
guide on writing trace configs.

WARNING: The below command does not work on Android P because the `--txt` option
was introduced in Q. The binary protobuf format should be used instead; the
details of this can be found on the
[_Trace configuration_ page](https://perfetto.dev/docs/concepts/config#pbtx-vs-binary-format).

If you are running on a Mac or Linux host, or are using a bash-based terminal on
Windows, you can use the following:

```bash
cat<<EOF>config.pbtx
duration_ms: 10000

buffers: {
    size_kb: 8960
    fill_policy: DISCARD
}
buffers: {
    size_kb: 1280
    fill_policy: DISCARD
}
data_sources: {
    config {
        name: "linux.ftrace"
        ftrace_config {
            ftrace_events: "sched/sched_switch"
            ftrace_events: "power/suspend_resume"
            ftrace_events: "sched/sched_process_exit"
            ftrace_events: "sched/sched_process_free"
            ftrace_events: "task/task_newtask"
            ftrace_events: "task/task_rename"
            ftrace_events: "ftrace/print"
            atrace_categories: "gfx"
            atrace_categories: "view"
            atrace_categories: "webview"
            atrace_categories: "camera"
            atrace_categories: "dalvik"
            atrace_categories: "power"
        }
    }
}
data_sources: {
    config {
        name: "linux.process_stats"
        target_buffer: 1
        process_stats_config {
            scan_all_processes_on_start: true
        }
    }
}
EOF

./record_android_trace -c config.pbtx -o trace_file.perfetto-trace
```

Or alternatively, when using directly the on-device command:

```bash
cat config.pbtx | adb shell perfetto -c - --txt -o /data/misc/perfetto-traces/trace.perfetto-trace
```

Alternatively, first push the trace config file and then invoke perfetto:

```bash
adb push config.pbtx /data/local/tmp/config.pbtx
adb shell 'cat /data/local/tmp/config.pbtx | perfetto --txt -c - -o /data/misc/perfetto-traces/trace.perfetto-trace'
```

NOTE: because of strict SELinux rules, on non-rooted builds of Android, passing
directly the file path as `-c /data/local/tmp/config` will fail, hence the
`-c -` + stdin piping above. From Android 12 (S), `/data/misc/perfetto-configs/`
can be used instead.

Pull the file using
`adb pull /data/misc/perfetto-traces/trace ~/trace.perfetto-trace` and open it
in the [Perfetto UI](https://ui.perfetto.dev).

NOTE: On devices before Android 10, adb cannot directly pull
`/data/misc/perfetto-traces`. Use
`adb shell cat /data/misc/perfetto-traces/trace > trace.perfetto-trace` to work
around.

The full reference for the `perfetto` cmdline interface can be found
[here](/docs/reference/perfetto-cli.md).

## Exclusive Tracing Sessions

Perfetto is designed to support [multiple concurrent tracing sessions](/docs/concepts/concurrent-tracing-sessions.md) from different sources (e.g., adb, on-device apps, automated testing). While this works for most data sources, some advanced features cannot be reliably multiplexed, and sensitive performance measurements require minimizing interference from other traces. In these situations, Perfetto requires a guarantee that no other tracing session is active.

To address this, Perfetto offers an "exclusive" mode. When a session is started in exclusive mode, it ensures no other sessions are running, providing a clean tracing environment. This is controlled by the `exclusive_prio` field in the `TraceConfig`.

### When to use exclusive mode

You should use an exclusive session in the following scenarios:

* For sensitive performance measurements where you need to minimize interference from other concurrent tracing activities.
* When using data sources with high overhead, such as `function_graph` in ftrace, to ensure their behavior is not affected by other sessions.
* When configuring parameters that apply globally and are not multiplexed, like the ftrace buffer size (`buffer_size_kb`), which is only configured by the first active session.
* When using specific ftrace features that modify global kernel state. As of Perfetto v52 (Android 25Q3+), these include:
    *   `tids_to_trace`: Filters ftrace events by specific Thread IDs (TIDs).
    *   `tracefs_options`: Control the tracers or the trace output via tracefs/trace_options
    *   `tracing_cpumask`: Restricts tracing to a specific set of CPUs.

### Behavior

An exclusive session has the following behavior:

*   **Priority System**: The `exclusive_prio` is an unsigned integer where a higher number indicates a higher priority. A session is only considered "exclusive" if its priority is greater than 0.
*   **Preemption**: If a new exclusive session is requested with a priority strictly higher than any other active session, it will be started, and all other existing sessions (both exclusive and non-exclusive) will be aborted. Consumers of aborted sessions will receive an error message (e.g., `Aborted due to user requested higher-priority (#priority) exclusive session.`).
*   **Blocking**: While an exclusive session is active, any attempt to start a new non-exclusive session, or an exclusive session with a lower or equal priority, will be rejected.
*   **Privileged Access**: On Android, requesting an exclusive session is a privileged operation and can only be done by `root` or `shell` users.

### How to enable

To start an exclusive session, add the `exclusive_prio` field to your trace config file.

This feature is available from Perfetto v52 and on Android from 25Q3+.

```protobuf
duration_ms: 10000

buffers: {
    size_kb: 8192
}

# Request an exclusive session with priority 10.
# This will abort any running sessions (provided they have a lower priority) and
# block new sessions with a lower or equal priority.
exclusive_prio: 10

data_sources: {
    config {
        name: "linux.ftrace"
        ftrace_config {
            # Advanced features like funcgraph can now be used more reliably.
            function_graph: true
            ftrace_events: "sched/sched_switch"
        }
    }
}
```
