# Converting from Perfetto to other trace formats

Perfetto's native protobuf trace format can be converted to other formats using
the `convert` subcommand of `trace_processor`.

> NOTE: This functionality used to live in a separate `traceconv` tool. That
> tool has been folded into `trace_processor`. The `traceconv` download still
> works as a back-compatible alias (it now fetches `trace_processor` and runs
> it in this mode), but new scripts and docs should use `trace_processor`.

![](/docs/images/traceconv-summary.png)

> To attach native symbols or ProGuard/R8 deobfuscation mappings to a trace,
> see [Symbolization and deobfuscation](/docs/learning-more/symbolization.md)
> instead (the `bundle` and `util` subcommands). This page covers only format
> conversion.

## Prerequisites

- A host running Linux, macOS or Windows
- Python 3 (only required if using the `trace_processor` wrapper script below; on
  Windows this also requires `curl`, which ships with Windows 10 and later)
- A Perfetto protobuf trace file

## Usage

To use the latest binaries:

<?tabs>

TAB: Linux / macOS

```bash
curl -LO https://get.perfetto.dev/trace_processor
chmod +x trace_processor
./trace_processor convert <format> [OPTIONS] [input_file] [output_file]
```

TAB: Windows

```powershell
curl.exe -LO https://get.perfetto.dev/trace_processor
python trace_processor convert <format> [OPTIONS] [input_file] [output_file]
```

</tabs?>

The `trace_processor` script is a thin Python wrapper that downloads and caches
the correct native binary for your platform under
`~/.local/share/perfetto/prebuilts` on first use.

`convert` reads from stdin and writes to stdout when the input or output paths
are omitted (or passed as `-`). Run `./trace_processor help convert` to print
the full list of formats and options supported by your version.

## Format conversion

| Format     | Output                                                       |
| ---------- | ------------------------------------------------------------ |
| `text`     | protobuf text format — a text representation of the protos   |
| `json`     | Chrome JSON format, viewable in `chrome://tracing`           |
| `systrace` | ftrace text/HTML format used by Android systrace             |
| `ctrace`   | compressed systrace format                                   |
| `profile`  | aggregated pprof profile (heapprofd, perf, Java heap graphs) |
| `firefox`  | Firefox profiler format                                      |

Examples:

```bash
./trace_processor convert json     trace.perfetto-trace trace.json
./trace_processor convert systrace trace.perfetto-trace trace.html
./trace_processor convert text     trace.perfetto-trace trace.textproto
```

`profile` writes one or more `.pb` files into a directory (a random tmp
directory by default) rather than a single output file, so use
`--output-dir` instead of a positional output path:

```bash
./trace_processor convert profile --output-dir ./profiles trace.perfetto-trace
./trace_processor convert profile --java-heap --pid 1234 --output-dir ./profiles trace.perfetto-trace
./trace_processor convert profile --perf --timestamps 1000000,2000000 --output-dir ./profiles trace.perfetto-trace
```

Common options:

- `--truncate start|end` (for `systrace`, `json`, `ctrace`): keep only the
  start or end of the trace.
- `--full-sort` (for `systrace`, `json`, `ctrace`): force full trace
  sorting.
- `--skip-unknown` (for `text`): skip unknown proto fields.
- `--alloc | --perf | --java-heap` (for `profile`): restrict to a single
  profile type (default: auto-detect).
- `--no-annotations` (for `profile`): do not add derived annotations to
  frames.
- `--pid` / `--timestamps` (for `profile`): filter by process or specific
  sample timestamps.
- `--output-dir DIR` (for `profile`): output directory for the generated
  pprof files.

For the inverse of `convert text` (turning a text-format trace back into
binary) and other low-level trace helpers, see `trace_processor help util`.

## Opening in the legacy systrace UI

If you just want to open a Perfetto trace with the legacy (Catapult) trace
viewer, you can just navigate to [ui.perfetto.dev](https://ui.perfetto.dev), and
use the _"Open with legacy UI"_ link. This runs the trace conversion within the
browser using WebAssembly and passes the converted trace seamlessly to
chrome://tracing.
