# PERFETTO(1)

## NAME

perfetto - capture traces

## DESCRIPTION

This section describes how to use the `perfetto` commandline binary to capture
traces. Examples are given in terms of an Android device connected over ADB.

`perfetto` has two modes for configuring the tracing session (i.e. what and how
to collect):

__lightweight mode__
: all config options are supplied as commandline flags,
  but the available data sources are restricted to ftrace and atrace. This mode
  is similar to
  [`systrace`](https://developer.android.com/topic/performance/tracing/command-line).

__normal mode__
: the configuration is specified in a protocol buffer. This allows for full
  customisation of collected traces.


## GENERAL OPTIONS

The following table lists the available options when using `perfetto` in either
mode.

`-d`, `--background`
:    Perfetto immediately exits the command-line interface and continues
     recording your trace in background.

`-D`, `--background-wait`
:    Like `--background`, but waits (up to 30s) for all data sources to be
     started before exiting.

`--notify-fd` _FD_
:    Like `--background-wait`, but instead of daemonizing and waiting before
     exiting, writes one status byte and closes the given file descriptor.
     Writes `0` on success, non-zero on timeout/error. Not supported on
     Windows.

`-o`, `--out` _OUT_FILE_
:    Specifies the desired path to the output trace file, or `-` for stdout.
     `perfetto` writes the output to the file described in the flags above.
     The output format compiles with the format defined in
     [AOSP `trace.proto`](/protos/perfetto/trace/trace.proto).

`--no-clobber`
:    Do not overwrite an existing output file.

`--clone` _TSID_
:    Creates a read-only clone of an existing tracing session identified by
     session id (see `--query`).

`--clone-by-name` _NAME_
:    Creates a read-only clone of an existing tracing session identified by
     `unique_session_name`.

`--clone-for-bugreport`
:    Can only be used with `--clone` or `--clone-by-name`. Disables
     `trace_filter` on the cloned session.

`--add-note` _key[=value]_
:    Adds a user note to the trace config. If `=value` is omitted, value is an
     empty string.

`--version`
:    Prints the `perfetto` version string and exits.

`--dropbox` _TAG_
:    Uploads your trace via the
     [DropBoxManager API](https://developer.android.com/reference/android/os/DropBoxManager.html)
     using the tag you specify. Android only. Deprecated: use `--upload`
     instead.

`--upload`
:    Uploads trace output to Android framework reporting paths configured in
     `TraceConfig` (`incident_report_config` or `android_report_config`).
     Android only.

`--alert-id` _ID_
:    Statsd metadata. ID of the alert that triggered this trace.

`--config-id` _ID_
:    Statsd metadata. ID of the triggering config.

`--config-uid` _UID_
:    Statsd metadata. UID of app which registered the triggering config.

`--subscription-id` _ID_
:    Statsd metadata. ID of the subscription that triggered this trace.

`--save-for-bugreport`
:    If a trace with `bugreport_score > 0` is running, saves it into a file and
     outputs the path when done.

`--save-all-for-bugreport`
:    Clones all eligible bugreport sessions and saves them into bugreport
     output files.

`--no-guardrails`
:     Disables protections against excessive resource usage when using
      `--upload` (testing only).


`--reset-guardrails`
:     Compatibility option. Guardrails no longer exist in `perfetto_cmd`; this
      option remains for backwards compatibility.

`--query`
:     Queries the service state and prints it as human-readable text.

`--long`
:     Expands some fields in `--query` output (for example category lists).
     Can only be used with `--query`.

`--query-raw`
:     Similar to `--query`, but prints raw proto-encoded bytes of
      `tracing_service_state.proto`.

`--detach` _KEY_
:     Detach from tracing session using the given key.

`--attach` _KEY_
:     Re-attach to a detached tracing session using the given key.

`--stop`
:     Stop tracing once re-attached. Supported only with `--attach`.

`--is_detached` _KEY_
:     Checks whether the session can be re-attached. Exit code semantics:
      `0` yes, `2` no, `1` error.

`-h`,  `--help`
:     Prints out help text for the `perfetto` tool.


## SIMPLE MODE

For ease of use, the `perfetto` command includes support for a subset of
configurations via command line arguments. On-device, these
configurations behave equivalently to the same configurations provided
by a *CONFIG_FILE* (see below).

The general syntax for using `perfetto` in *simple mode* is as follows:

```
 adb shell perfetto [ --time TIMESPEC ] [ --buffer SIZE ] [ --size SIZE ]
     [ --app APP_NAME ]
    [ ATRACE_CAT | FTRACE_GROUP/FTRACE_NAME]...
```


The following table lists the available options when using `perfetto` in
*simple mode*.

`-t`, `--time` _TIME[s|m|h]_
:    Specifies the trace duration in seconds, minutes, or hours.
     For example, `--time 1m` specifies a trace duration of 1 minute.
     The default duration is 10 seconds.

`-b`, `--buffer` _SIZE[mb|gb]_
:    Specifies the ring buffer size in megabytes (mb) or gigabytes (gb).
     The default parameter is `--buffer 32mb`.

`-s`, `--size` _SIZE[mb|gb]_
:    Specifies the max file size in megabytes (mb) or gigabytes (gb).
     By default `perfetto` uses only in-memory ring-buffer.

`-a`, `--app` _APP_NAME_
:    Specifies an Android app name for atrace app-level tracing.


This is followed by a list of event specifiers:

`ATRACE_CAT`
:    Specifies the atrace categories you want to record a trace for.
     For example, the following command traces Window Manager using atrace:
     `adb shell perfetto --out FILE wm`. To record other categories, see the
     [list of atrace categories](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/native/cmds/atrace/atrace.cpp).
     Note: Available categories are Android version dependent.

`FTRACE_GROUP/FTRACE_NAME`
:    Specifies the ftrace events you want to record a trace for.
     For example, the following command traces sched/sched_switch events:
     `adb shell perfetto --out FILE sched/sched_switch`


## NORMAL MODE

The general syntax for using `perfetto` in *normal mode* is as follows:

```
 adb shell perfetto [ --txt ] --config CONFIG_FILE
```

The following table lists the available options when using `perfetto` in
*normal* mode.

`-c`, `--config` _CONFIG_FILE_
:    Specifies the path to a configuration file. In normal mode, some
     configurations may be encoded in a configuration protocol buffer.
     This file must comply with the protocol buffer schema defined in AOSP
     [`trace_config.proto`](/protos/perfetto/config/trace_config.proto).
     You select and configure the data sources using the DataSourceConfig member
     of the TraceConfig, as defined in AOSP
     [`data_source_config.proto`](/protos/perfetto/config/data_source_config.proto).
     Use `-` to read config bytes from stdin.

`--txt`
:    Instructs `perfetto` to parse the config file as pbtxt. This flag is
     not a stable API and is not recommended for production.
