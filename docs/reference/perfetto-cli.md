# Perfetto CLI

This section describes how to use the `perfetto` commandline binary to capture
traces. Examples are given in terms of an Android device connected over ADB.

`perfetto` has two modes for configuring the tracing session (i.e. what and how
to collect):

* __lightweight mode__: all config options are supplied as commandline flags,
  but the available data sources are restricted to ftrace and atrace. This mode
  is similar to
  [`systrace`](https://developer.android.com/topic/performance/tracing/command-line).
* __normal mode__: the configuration is specified in a protocol buffer. This
  allows for full customisation of collected traces.


## General options

The following table lists the available options when using `perfetto` in either
mode.

|Option|Description|
|---|---|
| `--background \| -d` |Perfetto immediately exits the command-line interface and continues recording your trace in background.|
|`--out OUT_FILE \| -o OUT_FILE`|Specifies the desired path to the output trace file, or `-` for stdout. `perfetto` writes the output to the file described in the flags above. The output format compiles with the format defined in [AOSP `trace.proto`](/protos/perfetto/trace/trace.proto).|
|`--dropbox TAG`|Uploads your trace via the [DropBoxManager API](https://developer.android.com/reference/android/os/DropBoxManager.html) using the tag you specify.|
|`--no-guardrails`|Disables protections against excessive resource usage when enabling the `--dropbox` flag during testing.|
|`--reset-guardrails`|Resets the persistent state of the guardrails and exits (for testing).|
|`--query`|Queries the service state and prints it as human-readable text.|
|`--query-raw`|Similar to `--query`, but prints raw proto-encoded bytes of `tracing_service_state.proto`.|
|`--help \| -h`|Prints out help text for the `perfetto` tool.|


## Lightweight mode

The general syntax for using `perfetto` in *lightweight mode* is as follows:

<pre class="none">
 adb shell perfetto [ --time <var>TIMESPEC</var> ] [ --buffer <var>SIZE</var> ] [ --size <var>SIZE</var> ]
           [ <var>ATRACE_CAT</var> | <var>FTRACE_GROUP/FTRACE_NAME</var>]...
</pre>


The following table lists the available options when using `perfetto` in
*lightweight mode*.

|Option|Description|
|--- |--- |
|`--time TIME[s\|m\|h] \| -t TIME[s\|m\|h]`|Specifies the trace duration in seconds, minutes, or hours. For example, `--time 1m` specifies a trace duration of 1 minute. The default duration is 10 seconds.|
|`--buffer SIZE[mb\|gb] \| -b SIZE[mb\|gb`]|Specifies the ring buffer size in megabytes (mb) or gigabytes (gb). The default parameter is `--buffer 32mb`.|
|`--size SIZE[mb\|gb] \| -s SIZE[mb\|gb]`|Specifies the max file size in megabytes (mb) or gigabytes (gb). By default `perfetto` uses only in-memory ring-buffer.|


This is followed by a list of event specifiers:

|Event|Description|
|--- |--- |
|`ATRACE_CAT`|Specifies the atrace categories you want to record a trace for. For example, the following command traces Window Manager using atrace: `adb shell perfetto --out FILE wm`. To record other categories, see this [list of atrace categories](https://android.googlesource.com/platform/frameworks/native/+/refs/tags/android-q-preview-5/cmds/atrace/atrace.cpp#100).|
|`FTRACE_GROUP/FTRACE_NAME`|Specifies the ftrace events you want to record a trace for. For example, the following command traces sched/sched_switch events: `adb shell perfetto --out FILE sched/sched_switch`|


## Normal mode

The general syntax for using `perfetto` in *normal mode* is as follows:

<pre class="none">
 adb shell perfetto [ --txt ] --config <var>CONFIG_FILE</var>
</pre>

The following table lists the available options when using `perfetto` in *normal* mode.

|Option|Description|
|--- |--- |
|`--config CONFIG_FILE \| -c CONFIG_FILE`|Specifies the path to a configuration file. In normal mode, some configurations may be encoded in a configuration protocol buffer. This file must comply with the protocol buffer schema defined in [AOSP `trace_config.proto`](/protos/perfetto/config/data_source_config.proto). You select and configure the data sources using the DataSourceConfig member of the TraceConfig, as defined in [AOSP `data_source_config.proto`](/protos/perfetto/config/data_source_config.proto).|
|`--txt`|Instructs `perfetto` to parse the config file as pbtxt. This flag is experimental, and it's not recommended that you enable it for production.|
