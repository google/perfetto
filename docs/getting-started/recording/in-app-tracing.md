# Recording In-App Traces with Perfetto

_In this page, you'll learn how to use the Perfetto SDK collect a trace from a
C/C++ app which we instrument. You'll view the collected trace with the Perfetto
UI and query it's contents programatically with PerfettoSQL._

## Adding your first instrumentation

### Setup

<?tabs>

TAB: C++

GitHub project

TAB: C

GitHub project

</tabs?>

### Slices

<?tabs>

TAB: C++

Scoped slices

TAB: C

Begin/end at the end of each function

</tabs?>

### Counters

Annotate counter

## Collecting your first app trace

Use APIs to fetch a trace

## Visualizing your first app trace

Video: open trace in the UI

## Analysing your first app trace

Video: write queries in the UI

#### Testing the SDK integration in out-of-process tracing mode (system mode)

If you are using the Perfetto [tracing SDK](/docs/instrumentation/tracing-sdk)
and want to capture a fused trace that contains both system traces events and
your custom app trace events, you need to start the `traced` and `traced_probes`
services ahead of time and then use the `perfetto` cmdline client.

For a quick start, the [tools/tmux](/tools/tmux) script takes care of building,
setting up and running everything. As an example, let's look at the process
scheduling data, which will be obtained from the Linux kernel via the [ftrace]
interface.

[ftrace]: https://www.kernel.org/doc/Documentation/trace/ftrace.txt

1. Run the convenience script with an example tracing config (10s duration):

```bash
tools/tmux -c test/configs/scheduling.cfg -C out/linux -n
```

This will open a tmux window with three panes, one per the binary involved in
tracing: `traced`, `traced_probes` and the `perfetto` client cmdline.

2. Start the tracing session by running the pre-filled `perfetto` command in the
   down-most [consumer] pane.

3. Detach from the tmux session with `Ctrl-B D`,or shut it down with
   `tmux kill-session -t demo`. The script will then copy the trace to
   `/tmp/trace.perfetto-trace`, as a binary-encoded protobuf (see
   [TracePacket reference](/docs/reference/trace-packet-proto.autogen)).

## Next steps

Link to instrumentation guide for how to do more instrumentation with SDK.
