# Frequently Asked Questions

## How do I open trace in UI from command line?

When collecting traces from the command line, a convenient way to open traces
is to use the [open\_trace\_in\_ui script](/tools/open_trace_in_ui).

This can be used as follows:

```sh
curl -OL https://github.com/google/perfetto/raw/master/tools/open_trace_in_ui
chmod +x open_trace_in_ui
./open_trace_in_ui -i /path/to/trace
```

If you already have a Perfetto checkout, the first two steps can be skipped.
From the Perfetto root, run:

```sh
tools/open_trace_in_ui -i /path/to/trace
```

## Incorrectly displayed overlapping events in JSON trace

NOTE: JSON is considered a legacy trace format and is supported on a best-effort
basis.

The Perfetto UI and trace processor do support overlapping B/E/X events, in
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
