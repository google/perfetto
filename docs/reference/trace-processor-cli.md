# Trace Processor command-line reference

`trace_processor` loads, queries, converts, enriches, and serves traces. The
native executable is built as `trace_processor_shell`; the downloadable
`trace_processor` wrapper runs it with the same command-line arguments.

For installation and your first queries, see
[Analyzing traces from the command line](/docs/getting-started/command-line-analysis.md).
For the C++ library, see [Trace Processor](/docs/analysis/trace-processor.md).

## Synopsis

```text
trace_processor <command> [flags] [positional args]
trace_processor <trace_file>
trace_processor help <command>
```

With a trace file and no command, the tool opens an interactive SQL shell.
`--help` prints top-level help; `<command> --help` and `help <command>` print
command-specific help, including the flags supported by that build.

The classic flat-flag interface (`-q`, `-Q`, `--httpd`, `--summary`,
`--run-metrics`, `-e`, `--stdiod`) remains supported. Use `--help-classic`
for its flags.

## {#global-flags} Global flags (apply to every subcommand)

These flags are accepted in addition to the subcommand-specific flags below
and behave the same across all subcommands:

- **Help and version:** `-h, --help`, `-v, --version`.
- **Progress:** `--no-progress` disables live progress, preserving summaries,
  warnings, and errors.
- **Trace ingestion:** `--full-sort`, `--no-ftrace-raw`,
  `--analyze-trace-proto-content`, `--crop-track-events`.
- **PerfettoSQL packages:** `--add-sql-package PATH[@PKG]`,
  `--override-sql-package PATH[@PKG]`, `--override-stdlib PATH`
  (requires `--dev`).
- **Metric extensions:** `--metric-extension DISK_PATH@VIRTUAL_PATH`.
- **Auxiliary file content:** `--register-files-dir PATH` exposes the
  contents of files under `PATH` to importers (e.g. ETM decoders).
- **Development:** `--dev`, `--dev-flag KEY=VALUE`, `--extra-checks`.
- **Metatracing:** `-m, --metatrace FILE`, `--metatrace-buffer-capacity N`,
  `--metatrace-categories CATEGORIES`. This produces a Perfetto trace of
  trace processor itself, which you can load back into the UI for
  performance debugging.

## Progress and diagnostics

Diagnostics go to stderr. Live progress is displayed only when stderr is a
terminal and `TERM` is not `dumb`. Redirected stderr contains ordinary messages
without progress redraws. `--no-progress` suppresses live progress independently
of verbosity and color; summaries, warnings, and errors remain enabled.

## Color environment variables

| Environment | Behavior |
| --- | --- |
| Nonempty `FORCE_COLOR` | Force ANSI color, including when stderr is redirected. Takes precedence over `NO_COLOR`. |
| Nonempty `NO_COLOR` | Disable automatic color. |
| Neither | On POSIX, use terminal detection and disable automatic color for `TERM=dumb`. On Windows, automatic ANSI color is disabled. |

Empty values are ignored. Any nonempty value counts, including `0`, following
[FORCE_COLOR](https://force-color.org/) and [NO_COLOR](https://no-color.org/).
Forcing color does not enable progress redraws.

## {#subcommands} Commands

| Command | Purpose |
| --- | --- |
| [`query`](#subcommand-query) | Run SQL and print results. |
| [`interactive`](#subcommand-interactive) | Open a SQL prompt. |
| [`server`](#subcommand-server) | Serve traces over RPC or manage a session. |
| [`summarize`](#subcommand-summarize) | Compute trace summaries. |
| [`export`](#subcommand-export) | Export parsed trace data. |
| [`convert`](#subcommand-convert) | Convert a trace to another format. |
| [`bundle`](#subcommand-bundle) | Package a trace with symbols and deobfuscation data. |
| [`util`](#subcommand-util) | Run low-level trace utilities. |
| [`metrics`](#subcommand-metrics) | Run legacy v1 metrics. |

### {#subcommand-query} `query`: run SQL

`query` loads a trace, runs one or more `;`-separated SQL statements, prints
the results to stdout, and exits. SQL can be passed as an argument, read from
a file, or piped on stdin:

```bash
# Pass SQL as an argument.
trace_processor query trace.pftrace "SELECT ts, dur, name FROM slice LIMIT 5"

# Read SQL from a file.
trace_processor query -f queries.sql trace.pftrace

# Pipe SQL on stdin.
cat queries.sql | trace_processor query trace.pftrace
```

Each statement's result set is printed as CSV, and consecutive result sets
are separated by a single blank line. The separator is unambiguous because
every string value is quoted.

Flags:

- `--remote ADDR`: run against a warm session instead of loading a local
  trace; see [sessions](/docs/analysis/trace-processor.md#sessions). `ADDR` is a session name, a `*.sock` or
  absolute socket path, or `host:port`. No trace-file argument is passed in
  this mode.
- `-f, --query-file FILE`: read SQL from `FILE`; pass `-` to read from stdin.
- `-i, --interactive`: drop into the interactive REPL after the queries
  finish.
- `-W, --wide`: use double-width columns when printing results.
- `--perf-file FILE`: write trace-load and query timings to `FILE`.
- `--structured-query-id ID` plus `--summary-spec FILE` _(advanced)_: run a
  single structured query by ID from one or more
  [TraceSummarySpec](/docs/analysis/trace-summary.md) files, instead of the SQL sources
  above.

### {#subcommand-interactive} `interactive`: REPL

`interactive` opens the same interactive PerfettoSQL prompt described in the
[shell guide](/docs/analysis/trace-processor.md#shell). It is the default subcommand, so
`trace_processor trace.pftrace` and
`trace_processor interactive trace.pftrace` are equivalent. The only
subcommand-specific flag is `-W, --wide`.

### {#subcommand-server} `server`: HTTP, stdio, or unix RPC

`server` exposes trace processor over a remote-procedure-call protocol:

```bash
# HTTP server, used by ui.perfetto.dev. Listens on port 9001 by default.
trace_processor server http

# Pre-load a trace and serve it over HTTP.
trace_processor server http trace.pftrace

# stdio server: length-prefixed RPC for tooling that embeds
# trace_processor as a subprocess.
trace_processor server stdio

# Named unix-socket session: keeps the trace warm for repeated
# `query --remote <name>` calls (see the shell guide).
trace_processor server unix --name mysession --daemonize trace.pftrace

# Stop a unix session by name or socket path.
trace_processor server kill mysession
```

Flags:

- `--port PORT`: HTTP port (default 9001).
- `--ip-address IP`: HTTP bind address.
- `--additional-cors-origins O1,O2,...`: extra CORS-allowed origins on top
  of the defaults (`https://ui.perfetto.dev`, `http://localhost:10000`,
  `http://127.0.0.1:10000`).
- `--name NAME`: session name for unix mode (default: auto-generated).
- `--path PATH`: explicit socket path for unix mode (mutually exclusive
  with `--name`).
- `--daemonize`: detach into the background (unix mode, POSIX only).
- `--idle-timeout auto|DUR`: reap the server after this much inactivity
  (e.g. `30m`, `90s`); `auto` means 30 minutes for unix and never for
  http, `0`/`never` disables.
- `--idle-start auto|orphaned|last-query`: when the idle clock applies
  (default `auto`: owner-aware).

The trace file is optional in `http` and `unix` modes; clients can also
load traces remotely. The most common client is the Perfetto UI, which
auto-detects a local server and offloads trace parsing to it. See
[Visualising large traces](/docs/visualization/large-traces.md) for the
end-user flow, or
[trace_processor.proto](/protos/perfetto/trace_processor/trace_processor.proto)
for the RPC wire schema.

### {#subcommand-summarize} `summarize`: compute trace summaries

`summarize` computes a [trace summary](/docs/analysis/trace-summary.md). Pass the trace
file first, then any spec files; select built-in v2 metrics with
`--metrics-v2`:

```bash
# Run every available v2 metric.
trace_processor summarize --metrics-v2 all trace.pftrace

# Run two specific metrics defined in spec.textproto.
trace_processor summarize \
  --metrics-v2 startup_metric,memory_metric \
  trace.pftrace spec.textproto
```

Flags:

- `--metrics-v2 IDS`: comma-separated metric ids, or the literal `all`.
- `--metadata-query ID`: query id used to populate the summary's
  `metadata` field.
- `--format text|binary`: output format for the `TraceSummary` proto
  (default `text`).
- `--post-query FILE`: run this SQL file after summarization. When set, the
  summary proto is not printed; the SQL output is printed instead.
- `--perf-file FILE`: write load/query timings to `FILE`.
- `-i, --interactive`: drop into the REPL after summarization finishes.

Spec files are detected as binary or text by extension (`.pb` for binary,
`.textproto` for text), with content sniffing as a fallback.

### {#subcommand-export} `export`: write trace data to a file

`export` writes the parsed trace data to a file. The format is the first
positional argument, the output path is given with `-o`:

```bash
# Version-coupled archive, loadable by the same version of trace processor.
trace_processor export perfetto -o archive.tar trace.pftrace

# Static tables as standard Arrow files in a tar.
trace_processor export arrow_tar -o tables.tar trace.pftrace

# Static tables and views as a SQLite database.
trace_processor export sqlite -o trace.db trace.pftrace
```

Formats:

- **`perfetto`**: a version-coupled archive of the non-empty static tables.
  A fresh trace processor instance from the same version can load it back as
  a trace; a different version may load it, but this is not guaranteed. The
  only format that can be reloaded.
- **`arrow_tar`**: a tar of standard [Apache Arrow](https://arrow.apache.org/)
  files, one per statically registered table, including empty tables and
  implicit ID columns. Stable and forwards-compatible across versions, for
  external consumers (e.g. pandas, Polars, pyarrow). Cannot be loaded back
  into trace processor.
- **`sqlite`**: the statically registered tables plus the trace's views, as
  a SQLite database readable by any SQLite tool.

Flags:

- `-o, --output FILE`: output file path (required).

All three formats export the statically registered tables; only `sqlite` also
includes views. Runtime tables created during the session (e.g.
`CREATE PERFETTO TABLE`) are not exported. Exports stream to disk, so memory
use stays bounded for large traces. For task-oriented recipes, see
[Export trace data](/docs/getting-started/command-line-analysis.md#export-trace-data).

### {#subcommand-convert} `convert`: change trace format

```text
trace_processor convert <format> [flags] [input] [output]
```

Formats are `systrace`, `json`, `ctrace`, `text`, `profile`, and `firefox`.
Omitted input and output paths use stdin and stdout. For `profile`, use
`--output-dir` instead of an output-file argument. `--no-progress` applies
to conversion progress as well as trace loading. Run `help convert` for
format-specific options.

### {#subcommand-util} `util`: low-level trace utilities

```text
trace_processor util <utility> [flags] [positional args]
```

Utilities are `merge`, `symbolize`, `deobfuscate`, `decompress_packets`, and
`text_to_binary`. Run `help util` for their arguments. See the
[merging guide](/docs/analysis/merging-traces.md) and
[symbolization guide](/docs/learning-more/symbolization.md) for workflows.

### {#subcommand-metrics} `metrics`: legacy v1 metrics

Runs v1 metrics. For new workflows, use `summarize --metrics-v2`. Run
`help metrics` for supported flags, and see
[Trace-based metrics](/docs/analysis/metrics.md) for the legacy workflow.

### {#subcommand-bundle} `bundle`: enrich a trace

#### Synopsis

```text
trace_processor bundle [options] <input> <output>
```

Produces an enriched trace containing the input trace, native symbol packets,
and Java/Kotlin deobfuscation packets in a TAR archive. The Perfetto UI and
Trace Processor can open this archive directly.

For prerequisites and worked examples, see the
[symbolization guide](/docs/learning-more/symbolization.md#option-1-traceconv-bundle).
Run `trace_processor help bundle` for the complete list of accepted flags,
including common Trace Processor options.

#### Arguments

| Argument | Meaning |
| --- | --- |
| `input` | Input trace file path. Stdin is not supported. |
| `output` | Destination file path. Stdout is not supported. Its parent directory must exist and be writable. |

#### Options

- `--symbol-paths PATH1,PATH2,...`: additional directories to search for native
  symbols (in addition to the auto-discovered ones).
- `--no-auto-symbol-paths`: disable auto-discovery of native symbol paths.
  `--symbol-paths` and `PERFETTO_BINARY_PATH` remain active.
- `--proguard-map [pkg=]PATH`: additional ProGuard/R8 `mapping.txt` to apply for
  Java/Kotlin deobfuscation. Repeat the flag for multiple maps. The optional
  `pkg=` prefix scopes a map to a specific Java package.
- `--no-auto-proguard-maps`: disable auto-discovery of ProGuard/R8 mapping files
  (e.g. the standard Android Gradle layout). Only maps given via
  `--proguard-map` are applied.
- `--verbose`: print every path tried and every library looked up &mdash; useful
  when debugging "could not find" errors.

#### {#symbol-paths} Native symbol paths

A symbol path is a directory containing native binaries with symbols, separate
native debug files, or Breakpad symbol files. It is not a source-code directory
or a ProGuard/R8 mapping file. Native binaries must match the build IDs recorded
in the trace; rebuilding the same source does not necessarily produce a match.

`bundle` recursively indexes native binaries under the configured directories
and matches them by build ID. Their directory layout and filenames need not
match the paths recorded in the trace. For Breakpad, each configured directory
is searched for `<build-id>.breakpad` (the build ID encoded as lowercase hex).

The directory list is assembled from:

1. The comma-separated `--symbol-paths` argument.
2. `PERFETTO_BINARY_PATH`, separated by `:` on POSIX or `;` on Windows.
3. Automatically discovered directories, unless `--no-auto-symbol-paths` is set.

Automatic directories are added in this order, when they exist:

| Directory | Source |
| --- | --- |
| `/usr/lib/debug` | System debug files. |
| `$HOME/.debug` | Per-user debug files. |
| `$ANDROID_PRODUCT_OUT/symbols` | AOSP build output. |
| `./app/build/intermediates/cmake` | Gradle CMake output, relative to the working directory. |
| `./app/build/intermediates/merged_native_libs` | Gradle native libraries, relative to the working directory. |
| `./.build-id` | Local build-ID directory, relative to the working directory. |

With automatic discovery enabled, absolute Unix-style binary paths recorded in
`stack_profile_mapping` are also considered as individual files on the host.
`--no-auto-symbol-paths` disables both these files and the automatic directories;
it does not disable `PERFETTO_BINARY_PATH`.

The order above describes how paths are collected, not a guaranteed preference
between duplicate copies of the same build ID during recursive indexing. Prefer
directories containing the matching unstripped or debug binaries rather than
mixing stripped and unstripped copies. Use `--verbose` to inspect lookup details.

#### Exit status

A successfully written bundle exits with status 0, including when some symbols
are unavailable. The symbolization summary reports missing symbols. Invalid
arguments and failures to produce the bundle exit with a nonzero status.
