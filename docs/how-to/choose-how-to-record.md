# Choose how to record

Pick the right tool for recording a Perfetto trace, based on your platform,
what is installed and how much control you need.

## Compare the options

| Option | Best for | Platforms | Needs | What you get |
| --- | --- | --- | --- | --- |
| **Record page** in [ui.perfetto.dev](https://ui.perfetto.dev) | First traces; picking data sources from a GUI | Android, Linux, Chrome, ChromeOS | Android 10+ with `traced` running (on by default from 11) over WebUSB, ADB + WebSocket or ADB + WebDeviceProxy; Linux needs `traced` plus `tracebox websocket_bridge`; Chrome needs the Perfetto UI extension | Trace opens in the UI when recording stops; the config can be saved, shared or exported |
| **`tools/record_android_trace`** | Repeatable recordings from a host shell or script | Android | Python 3, `adb`; Android 6+ (below 10 it sideloads `tracebox`) | Pushes the config, records, pulls the file and opens it in the UI (`-n` to skip) |
| **On-device `perfetto`** | Full control on the device; background, detached or cloned sessions | Android 9+ (enable `traced` on 9 and 10) | `adb shell`; config piped on stdin (`-c -`) | A file under `/data/misc/perfetto-traces` that you pull yourself |
| **`tracebox`** | System traces on Linux without installed daemons | Linux (also sideloaded on old Android) | One static binary; root, or write access to tracefs | Starts `traced` and `traced_probes` for the session; same flags as `perfetto` |
| **Tracing SDK, in-process backend** | An app recording only its own events | Any platform the SDK builds on | Linking the SDK; no daemons | App-controlled start and stop; the app writes the trace file |
| **Android Studio, ProfilingManager, System Tracing app** | App developers who want a curated workflow | Android | Android Studio, API 35+ for ProfilingManager, or Developer options on device | Perfetto traces with fewer options exposed |

NOTE: The Record page's WebUSB transport needs exclusive access to the device,
so stop any `adb` server first. If you use `adb` alongside the UI, use the
ADB + WebSocket transport instead.

## Pick by situation

- You want a first trace on Android with no setup → use the Record page. See
  [Record your first system trace](/docs/getting-started/system-tracing.md).
- You want a first trace on Linux → download `tracebox` and pass it a config.
  See [Record your first system trace](/docs/getting-started/system-tracing.md)
  and [tracebox](/docs/reference/tracebox.md).
- You record the same config often, or from a script → use
  `record_android_trace -c config.pbtx`. See
  [record_android_trace](/tools/record_android_trace).
- You need a flag the wrappers do not expose, or you are on a device without a
  host → run `perfetto` on the device. See
  [Record from the command line on Android](/docs/learning-more/android.md) and
  [perfetto](/docs/reference/perfetto-cli.md).
- You want to start tracing, disconnect, and collect the file later → use
  `perfetto --background-wait`. See
  [Record in the background](/docs/learning-more/tracing-in-background.md).
- The trace must run for minutes or hours → stream it to a file with
  `write_into_file`. See
  [Trace configuration: streaming long traces](/docs/concepts/config.md#long-traces).
- You want a flight recorder that saves the trace only when something happens →
  use triggers. See
  [Trace configuration: triggers](/docs/concepts/config.md#triggers).
- You want to look at a running ring-buffer trace repeatedly without stopping it
  (Android 14+) → use `--clone-by-name`. See
  [Take periodic trace snapshots](/docs/getting-started/periodic-trace-snapshots.md).
- You need to trace Android boot (Android 13+) → arm a boot trace. See
  [Record boot traces and heap dumps on OOM](/docs/getting-started/local-android-trace-recording.md#boot-tracing).
- You want your app's events only, with no system daemons → use the SDK's
  in-process backend. See
  [Instrument a C++ app](/docs/getting-started/in-app-tracing.md).
- You want your app's events next to scheduling and other system data → use
  the SDK's system backend and record with any of the system tools above.
  See [Instrument a C++ app](/docs/getting-started/in-app-tracing.md).
- You want to trace desktop Chrome → use the Record page with the Chrome
  extension. See [Record a Chrome trace](/docs/getting-started/chrome-tracing.md).
- You need one trace across two Linux machines → use `traced_relay`. See
  [Trace multiple machines](/docs/learning-more/multi-machine-tracing.md).
- You are an Android app developer and prefer IDE or in-app tooling → use the
  [Android Studio Profiler](https://developer.android.com/studio/profile),
  [ProfilingManager](https://developer.android.com/reference/android/os/ProfilingManager)
  or the
  [System Tracing app](https://developer.android.com/topic/performance/tracing/on-device).
  All produce traces you can open in the Perfetto UI.

## Related

- [Trace configuration](/docs/concepts/config.md)
- [Choose a profiler](/docs/how-to/choose-a-profiler.md)
- [Instrument a C++ app](/docs/getting-started/in-app-tracing.md)
- [Supported trace and profile formats](/docs/getting-started/other-formats.md)
