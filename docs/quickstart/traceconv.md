# Quickstart: Trace conversion

_This quickstart demonstrates how Perfetto traces can be converted into other trace formats using the `traceconv` tool._

![](/docs/images/traceconv-summary.png)

## Prerequisites

- A host running Linux or MacOS
- A Perfetto protobuf trace file

The supported output formats are:

- `text` - protobuf text format: a text based representation of protos
- `json` - Chrome JSON format: the format used by chrome://tracing
- `systrace`: the ftrace text format used by Android systrace
- `profile` (heap profiler only): pprof-like format. This is only valid for
  traces with [native heap profiler](/docs/data-sources/native-heap-profiler.md)
  dumps.

## Setup

```bash
curl -LO https://get.perfetto.dev/traceconv
chmod +x traceconv
./traceconv [text|json|systrace|profile] [input proto file] [output file]
```

## Converting to systrace text format

`./traceconv systrace [input proto file] [output systrace file]`

## Converting to Chrome Tracing JSON format

`./traceconv json [input proto file] [output json file]`

## Opening in the legacy systrace UI

If you just want to open a Perfetto trace with the legacy (Catapult) trace
viewer, you can just navigate to [ui.perfetto.dev](https://ui.perfetto.dev),
and use the the _"Open with legacy UI"_ link. This runs `traceconv` within
the browser using WebAssembly and passes the converted trace seamlessly to
chrome://tracing.
