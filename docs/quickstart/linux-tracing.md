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
_If the script fails with SSL errors, try invoking it as `python3 tools/install-build-deps`, or upgrading your openssl libraries._

3. Generate all most common GN build configurations:
```bash
tools/build_all_configs.py
```

4. Build the Linux tracing binaries (On Linux it uses a hermetic clang toolchain, downloaded as part of step 2):
```bash
tools/ninja -C out/linux_clang_release traced traced_probes perfetto
```
_This step is optional when using the convenience `tools/tmux` script below._

## Capturing a trace

Due to Perfetto's [service-based architecture](/docs/concepts/service-model.md),
in order to capture a trace, the `traced` (session daemon) and `traced_probes`
(probes and ftrace-interop daemon) need to be running.

For a quick start, the [tools/tmux](/tools/tmux) script takes care of building,
setting up and running everything.
As an example, let's look at the process scheduling data, which will be obtained
from the Linux kernel via the [ftrace] interface.

[ftrace]: https://www.kernel.org/doc/Documentation/trace/ftrace.txt

1. Run the convenience script with an example tracing config (10s duration):
```
OUT=out/linux_clang_release CONFIG=test/configs/scheduling.cfg tools/tmux -n
```
This will open a tmux window with three panes, one per the binary involved in
tracing: `traced`, `traced_probes` and the `perfetto` client cmdline.

2. Start the tracing session by running the pre-filled `perfetto` command in
   the down-most [consumer] pane.

3. Detach from the tmux session with `Ctrl-B D`,or shut it down with
   `tmux kill-session -t demo`. The script will then copy the trace to
   `/tmp/trace.protobuf`, as a binary-encoded protobuf (see
   [TracePacket reference](/docs/reference/trace-packet-proto.autogen)).

## Visualizing the trace

We can now explore the captured trace visually by using a dedicated web-based UI.

NOTE: The UI runs fully in-browser using JavaScript + Web Assembly. The trace
      file is **not** uploaded anywhere by default, unless you explicitly click
      on the 'Share' link.

1. Navigate to [ui.perfetto.dev](https://ui.perfetto.dev) in a browser.

2. Click the **Open trace file** on the left-hand menu, and load the captured
   trace (by default at `/tmp/trace.protobuf`).

3. Explore the trace by zooming/panning using WASD, and mouse for expanding
   process tracks (rows) into their constituent thread tracks.
   Press "?" for further navigation controls.

Alternatively, you can explore the trace contents issuing SQL queries through 
the [trace processor](/docs/analysis/trace-processor).
