# Frequently Asked Questions

This page contains some common questions that the Perfetto team is asked
and their answers.

- [Frequently Asked Questions](#frequently-asked-questions)
  - [How do I open trace in UI from command line?](#how-do-i-open-trace-in-ui-from-command-line)
  - [Incorrectly displayed overlapping events in JSON trace](#incorrectly-displayed-overlapping-events-in-json-trace)

## How do I open trace in UI from command line?

When collecting traces from the command line, a convenient way to open traces
is to use the [open\_trace\_in\_ui script](/tools/open_trace_in_ui).

This can be used as follows:

```sh
curl -OL https://github.com/google/perfetto/raw/master/tools/open_trace_in_ui
chmod +x open_trace_in_ui
./open_trace_in_ui -i /path/to/trace
```

If you already have a Perfetto checkout, the first steps can be skipped.
From the Perfetto root, run:

```sh
tools/open_trace_in_ui -i /path/to/trace
```

## Incorrectly displayed overlapping events in JSON trace

Perfetto UI doesn't support overlapping B/E/X events, as per
[JSON spec](https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/preview#heading=h.nso4gcezn7n1).
Those events can only have nesting. Use B/E events in JSON which do support overlapping events on a single track.
Note that JSON traces are considered a legacy trace format and are supported on a best-effort basis.

It's recommended to use protobufs with [TrackEvents](https://perfetto.dev/docs/instrumentation/track-events) as a trace type.
