# System Tracing on Android

This page explains dives deeper into record system traces on Android building on
the introduction in the
[System Tracing Tutorial](/docs/getting-started/system-tracing.md).

## Starting the tracing services

Perfetto is based on [platform services](/docs/concepts/service-model.md) that
are available since Android 9 (P) but are enabled by default only since Android
11 (R).

On Android 9 (P) and 10 (Q) you need to do the following to ensure that the
tracing services are enabled before getting started:

```bash
# Needed only on Android 9 (P) and 10 (Q) on non-Pixel phones.
adb shell setprop persist.traced.enable 1
```

If you are running a version of Android older than P, the only supported way to
trace with Perfetto is with the `record_android_trace` script as described in
the
[System Tracing Tutorial](/docs/getting-started/system-tracing.md).

## Recording using the on-device /system/bin/perfetto command

The `record_android_trace` in the `System Tracing Tutorial` is really just a
wrapper around the `/system/bin/perfetto` binary which lives on device.

`/system/bin/perfetto` can be used directly without the script:

```bash
adb shell perfetto \
  # The path to the output file on device.
  -o /data/misc/perfetto-traces/trace_file.perfetto-trace \
  # Time to record the trace.
  -t 20s \
  # The atrace categories to record.
  sched freq idle am wm gfx view binder_driver hal dalvik input res memory
```

Caveats when using directly the `adb shell perfetto` workflow:

- Ctrl+C, which normally causes a graceful termination of the trace, is not
  propagated by ADB when using `adb shell perfetto` but only when using an
  interactive PTY-based session via `adb shell`.
- On non-rooted devices before Android 12, the config can only be passed as
  `cat config | adb shell perfetto -c -` (-: stdin) because of over-restrictive
  SELinux rules. Since Android 12 `/data/misc/perfetto-configs` can be used for
  storing configs.
- On devices before Android 10, adb cannot directly pull
  `/data/misc/perfetto-traces`. Use
  `adb shell cat /data/misc/perfetto-traces/trace > trace` to work around.
- When capturing longer traces, e.g. in the context of benchmarks or CI, use
  `PID=$(perfetto --background)` and then `kill $PID` to stop.

## Recording using the full trace config

The short syntax allows to enable only a subset of the data sources; for full
control of the trace config, pass the full trace config in input.

See the [_Trace configuration_ page](/docs/concepts/config.md) and the examples
in each data source doc page for detailed instructions about how to configure
all the various knobs of Perfetto.

If you are running on a Mac or Linux host, or are using a bash-based terminal on
Windows, you can use the following:

WARNING: The below command does not work on Android P because the `--txt` option
was introduced in Q. The binary protobuf format should be used instead; the
details of this can be found on the
[_Trace configuration_ page](https://perfetto.dev/docs/concepts/config#pbtx-vs-binary-format).

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
