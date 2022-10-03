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
- `profile` : pprof-like format. Either for traces with with
  [native heap profiler](/docs/data-sources/native-heap-profiler.md) dumps or
  [callstack sampling](/docs/quickstart/callstack-sampling.md) (note however
  callstacks requires the `--perf` flag).

## Setup

To use the latest binaries:

```bash
curl -LO https://get.perfetto.dev/traceconv
chmod +x traceconv
./traceconv [text|json|systrace|profile] [input proto file] [output file]
```

For versioned downloads, replace `<tag>` with the required git tag:

```bash
curl -LO https://raw.githubusercontent.com/google/perfetto/<tag>/tools/traceconv
chmod +x traceconv
./traceconv [text|json|systrace|profile] [input proto file] [output file]
```

## Converting to systrace text format

`./traceconv systrace [input proto file] [output systrace file]`

## Converting to Chrome Tracing JSON format

`./traceconv json [input proto file] [output json file]`

## Converting to pprof profile.

This extract all samples from the trace, and outputs a proto that is compatible
with pprof.

If you are extracting heaps profiles like heapprofd you can use the following:

`~/traceconv profile [input proto file] [output file]`

However if you are using callstack sampling like traced_perf then use the
following instead:

`~/traceconv profile [input proto file] [output file] --perf`

Note for `--perf` the output is one pprof file per process sampled in the trace.
You can use pprof to merge them together if desired.

## Opening in the legacy systrace UI

If you just want to open a Perfetto trace with the legacy (Catapult) trace
viewer, you can just navigate to [ui.perfetto.dev](https://ui.perfetto.dev),
and use the _"Open with legacy UI"_ link. This runs `traceconv` within
the browser using WebAssembly and passes the converted trace seamlessly to
chrome://tracing.
