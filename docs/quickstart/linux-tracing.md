# Quickstart: Record traces on Linux

Perfetto can capture system traces on Linux. All ftrace-based data sources
and most other procfs / sysfs-based data sources are supported.

Currently there are no packages or prebuilts for Linux. In order to run Perfetto
on Linux you need to build it from source.

## Building from source

1. Check out the code:
```bash
git clone https://android.googlesource.com/platform/external/perfetto/ && cd perfetto
```

2. Download and extract build dependencies:
```bash
tools/install-build-deps
```
_If the script fails with SSL errors, try upgrading your openssl package._

3. Generate the build configuration
```bash
tools/gn gen --args='is_debug=false' out/linux
# Or use `tools/setup_all_configs.py` to generate more build configs.
```

4. Build the Linux tracing binaries (On Linux it uses a hermetic clang toolchain, downloaded as part of step 2):
```bash
tools/ninja -C out/linux tracebox traced traced_probes perfetto 
```

## Capturing a trace

Due to Perfetto's [service-based architecture](/docs/concepts/service-model.md),
in order to capture a trace, the `traced` (session daemon) and `traced_probes`
(probes and ftrace-interop daemon) need to be running.
As per Perfetto v16, the `tracebox` binary bundles together all the binaries you
need in a single executable (a bit like `toybox` or `busybox`).

#### Capturing a trace with ftrace and /proc pollers, no SDK

If you are interested in overall system tracing and are not interested in
testing the SDK, you can use `tracebox` in autostart mode as follows:

```bash
out/linux/tracebox -o trace_file.perfetto-trace --txt -c test/configs/scheduling.cfg
```

#### Testing the SDK integration in out-of-process tracing mode (system mode)

If you are using the Perfetto [tracing SDK](/docs/instrumentation/tracing-sdk)
and want to capture a fused trace that contains both system traces events and
your custom app trace events, you need to start the `traced` and `traced_probes`
services ahead of time and then use the `perfetto` cmdline client.

For a quick start, the [tools/tmux](/tools/tmux) script takes care of building,
setting up and running everything.
As an example, let's look at the process scheduling data, which will be obtained
from the Linux kernel via the [ftrace] interface.

[ftrace]: https://www.kernel.org/doc/Documentation/trace/ftrace.txt

1. Run the convenience script with an example tracing config (10s duration):
```bash
tools/tmux -c test/configs/scheduling.cfg -C out/linux -n
```
This will open a tmux window with three panes, one per the binary involved in
tracing: `traced`, `traced_probes` and the `perfetto` client cmdline.

2. Start the tracing session by running the pre-filled `perfetto` command in
   the down-most [consumer] pane.

3. Detach from the tmux session with `Ctrl-B D`,or shut it down with
   `tmux kill-session -t demo`. The script will then copy the trace to
   `/tmp/trace.perfetto-trace`, as a binary-encoded protobuf (see
   [TracePacket reference](/docs/reference/trace-packet-proto.autogen)).

## Visualizing the trace

We can now explore the captured trace visually by using a dedicated web-based UI.

NOTE: The UI runs in-browser using JavaScript + Web Assembly. The trace
      file is **not** uploaded anywhere by default, unless you explicitly click
      on the 'Share' link. The 'Share' link is available only to Googlers.

1. Navigate to [ui.perfetto.dev](https://ui.perfetto.dev) in a browser.

2. Click the **Open trace file** on the left-hand menu, and load the captured
   trace (by default at `/tmp/trace.perfetto-trace`).

3. Explore the trace by zooming/panning using WASD, and mouse for expanding
   process tracks (rows) into their constituent thread tracks.
   Press "?" for further navigation controls.

Alternatively, you can explore the trace contents issuing SQL queries through
the [trace processor](/docs/analysis/trace-processor).
