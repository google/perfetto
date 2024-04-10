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

## Why does Perfetto not support \<some obscure JSON format feature\>?

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

## Why are overlapping events in JSON traces not displayed correctly?

The Perfetto UI and trace processor do not support overlapping B/E/X events, in
compliance with the
[JSON spec](https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/preview#heading=h.nso4gcezn7n1).
As stated in the spec, events are only allowed to perfecty nest.

Users are recommended to emit
[TrackEvent](/docs/instrumentation/track-events.md)
instead, Perfetto's native trace format. See
[this guide](/docs/reference/synthetic-track-event.md) for how common JSON
events can be represented using
TrackEvent.

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
