# Converting from Perfetto to other trace formats

Perfetto's native protobuf trace format can be converted to other formats using
the `traceconv` utility.

![](/docs/images/traceconv-summary.png)

## Prerequisites

- A host running Linux or MacOS
- A Perfetto protobuf trace file

The supported output formats are:

- `text` - protobuf text format: a text based representation of protos
- `json` - Chrome JSON format: the format used by chrome://tracing
- `systrace`: the ftrace text format used by Android systrace
- `profile` : aggregated profile in the [pprof](https://github.com/google/pprof)
  format. Supports allocator profiles (heapprofd), perf profiles, and android
  java heap graphs.

## Usage

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

## Opening in the legacy systrace UI

If you just want to open a Perfetto trace with the legacy (Catapult) trace
viewer, you can just navigate to [ui.perfetto.dev](https://ui.perfetto.dev), and
use the _"Open with legacy UI"_ link. This runs `traceconv` within the browser
using WebAssembly and passes the converted trace seamlessly to chrome://tracing.
