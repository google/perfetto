# Frequently Asked Questions

## How do I open trace in UI from command line?

When collecting traces from the command line, a convenient way to open traces
is to use the [open\_trace\_in\_ui script](/tools/open_trace_in_ui).

This can be used as follows:

```sh
curl -OL https://github.com/google/perfetto/raw/main/tools/open_trace_in_ui
chmod +x open_trace_in_ui
./open_trace_in_ui -i /path/to/trace
```

If you already have a Perfetto checkout, the first two steps can be skipped.
From the Perfetto root, run:

```sh
tools/open_trace_in_ui -i /path/to/trace
```

## {#why-does-perfetto-not-support-some-obscure-json-format-feature} Why does Perfetto not support \<some obscure JSON format feature\>?

The JSON trace format is considered a legacy trace format and is supported on a
best-effort basis. While we try our best to maintain compatibility with the
chrome://tracing UI and the [format spec](https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/preview#heading=h.nso4gcezn7n1)
in how events are parsed and displayed, this is not always possible.
This is especially the case for traces which are programmatically generated
outside of Chrome and depend on the implementation details of chrome://tracing.

If supporting a feature would introduce a misproportional amount of technical
debt, we generally make the choice not to support that feature. Users
are recommended to emit [TrackEvent](/docs/instrumentation/track-events.md)
instead, Perfetto's native trace format. See
[this guide](/docs/reference/synthetic-track-event.md) for how common JSON
events can be represented using
TrackEvent.

## {#why-are-overlapping-events-in-json-traces-not-displayed-correctly} Why are overlapping events in JSON traces not displayed correctly?

A thread or process track in Perfetto is a strictly nested stack of slices:
slices must nest, they cannot partially overlap. This follows the
[JSON spec](https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/preview#heading=h.nso4gcezn7n1),
where duration (`B`/`E`/`X`) events are only allowed to nest. `chrome://tracing`
ignores this and draws overlaps anyway, but the result is broken underneath.
Perfetto keeps the model strict and, by default, drops overlapping complete
(`X`) events, reported as the `slice_drop_overlapping_complete_event` error.

Use cases and discussion are tracked in
[issue #4280](https://github.com/google/perfetto/issues/4280). What to do next
depends on whether you write the trace.

### If you write the trace

Emit the overlapping work as async events, which exist for exactly this case.
Use `b`/`e` events that share an `id` (or `id2`) so they land on one track:

```json
{
  "traceEvents": [
    {"ph":"b","cat":"gpu","name":"A","pid":0,"tid":7,"ts":3,"id2":{"local":"0x1"}},
    {"ph":"b","cat":"gpu","name":"B","pid":0,"tid":7,"ts":5,"id2":{"local":"0x1"}},
    {"ph":"e","cat":"gpu","name":"A","pid":0,"tid":7,"ts":6,"id2":{"local":"0x1"}},
    {"ph":"e","cat":"gpu","name":"B","pid":0,"tid":7,"ts":7,"id2":{"local":"0x1"}}
  ]
}
```

For higher fidelity, write Perfetto's native
[TrackEvent](/docs/instrumentation/track-events.md) format;
[this guide](/docs/reference/synthetic-track-event.md) maps common JSON events
onto it. If you're not sure how to represent your trace, file a Perfetto bug
linking [issue #4280](https://github.com/google/perfetto/issues/4280) and we'll
help.

### If you only consume traces from a tool you don't control

You can't change the data model, but you can:

- File a bug against the tool that writes the trace, asking it to emit async
  events, and link
  [issue #4280](https://github.com/google/perfetto/issues/4280).
- Turn on the *Preserve overlapping events in JSON traces* flag in the UI Flags
  page and reload. Overlapping events are then kept on extra depths of the
  thread track instead of being dropped. It's a workaround, not a faithful
  representation, so it's off by default.

Either way, add a note to
[issue #4280](https://github.com/google/perfetto/issues/4280) saying whether the
default or the workaround works for you.

## How can I use Perfetto tooling without instrumenting my program?

A common problem is that users want to use Perfetto analysis and visualization
tooling but they don't want to instrument their program. This can be because
Perfetto is not a good fit for their use-case or because they may already have
an existing tracing system.

The recommended approach for this is to emit Perfetto's native TrackEvent proto
format. A reference guide for this is available
[here](/docs/reference/synthetic-track-event.md).


## My app has multiple processes. How can see all of them in the same trace?

Use the [Tracing SDK](/docs/instrumentation/tracing-sdk.md#system-mode) in
"system mode". All processes will connect to `traced` over a socket and traced
will emit one trace with all processes.
