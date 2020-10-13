# Quickstart: Record traces on Android

`perfetto` allows you to collect system-wide performance traces from Android
devices from a variety of data sources (kernel scheduler via ftrace, userspace
instrumentation via atrace and all other data sources listed in this site).

## Starting the tracing services

Due to Perfetto's [service-based architecture](/docs/concepts/service-model.md)
, the `traced` and `traced_probes` services need to be running to record traces.

These services are shipped on Android system images by default since Android 9
(Pie) but are not always enabled by default.
On Android 9 (P) and 10 (Q) those services are enabled by default only on Pixel
phones and must be manually enabled on other phones.
Since Android 11 (R), perfetto services are enabled by default on most devices.

To enable perfetto services run:

```bash
# Will start both traced and traced_probes.
adb shell setprop persist.traced.enable 1
```

## Recording a trace

You can collect a trace in the following ways:

* Through the record page in the [Perfetto UI](https://ui.perfetto.dev).
* Using the `perfetto` command line interface [[reference](/docs/reference/perfetto-cli.md)].

### Perfetto UI

Navigate to ui.perfetto.dev and select **Record new trace**.

From this page, select and turn on the data sources you want to include in the trace. More detail about the different data sources can be found in the
_Data sources_ section of the docs.

![Record page of the Perfetto UI](/docs/images/record-trace.png)

If you are unsure, start by turning on **Scheduling details** under the **CPU** tab.

Ensure your device is connected and select **Add ADB device**. Once your device has successfully paired (you may need to allow USB debugging on the device), select the **Start Recording** button.

Allow time for the trace to be collected (10s by default) and then you should see the trace appear.

![Perfetto UI with a trace loaded](/docs/images/trace-view.png)

Your trace may look different depending on which data sources you enabled.

### Perfetto cmdline

#### Short syntax

If you are already familiar with `systrace` or `atrace`, there is an equivalent syntax with `perfetto`:

```bash
adb shell perfetto -o /data/misc/perfetto-traces/trace -t 20s sched freq idle am wm gfx view
```

#### Full trace config

The short syntax allows to enable only a subset of the data sources; for full
control of the trace config, pass the full trace config in input.

See the [_Trace configuration_ page](/docs/concepts/config.md) and the examples
in each data source doc page for detailed instructions about how to configure
all the various knobs of Perfetto.

If you are running on a Mac or Linux host, or are using a bash-based terminal
on Windows, you can use the following:

WARNING: The below command does not work on Android P because the `--txt` option
was introduced in Q. The binary protobuf format should be used instead; the
details of this can be found on the
[_Trace configuration_ page](https://perfetto.dev/docs/concepts/config#pbtx-vs-binary-format).

```bash
adb shell perfetto \
  -c - --txt \
  -o /data/misc/perfetto-traces/trace \
<<EOF
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
```

In all other cases, first push the trace config file and then invoke perfetto:
```bash
adb push config.txt /data/local/tmp/trace_config.txt
adb shell 'cat /data/local/tmp/trace_config.txt | perfetto --txt -c - -o /data/misc/perfetto-traces/trace'
```

NOTE: because of strict SELinux rules, on non-rooted builds of Android, passing
directly the file path as `-c /data/local/tmp/config` will fail, hence the
`-c -` + stdin piping above.

Pull the file using `adb pull /data/misc/perfetto-traces/trace ~/trace.pftrace`
and upload to the [Perfetto UI](https://ui.perfetto.dev).

The full reference for the `perfetto` cmdline interface can be found
[here](/docs/reference/perfetto-cli.md).

## On-device System Tracing app

Since Android 9 (P) it's possible to collect a trace directly from the device
using the System Tracing app, from Developer Settings.

See https://developer.android.com/topic/performance/tracing/on-device for
instructions.
