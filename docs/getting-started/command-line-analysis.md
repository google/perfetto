# Cookbook: Analyzing Traces from the Command Line

This page is a set of task-oriented recipes for working with traces from a
shell using `trace_processor`: running queries, iterating without
re-parsing, merging, exporting and converting. It shows the common form of
each task; the full list of subcommands and flags is in the
[Trace Processor reference](/docs/analysis/trace-processor.md).

## Get the binary

```bash
curl -LO https://get.perfetto.dev/trace_processor
chmod +x ./trace_processor
```

This is a thin Python wrapper that fetches and caches the right native
binary for your platform on first use (Windows and other options:
[reference](/docs/analysis/trace-processor.md#shell)).

## Run a query

`query` loads a trace, runs one or more `;`-separated SQL statements, and
prints each result set as CSV (blank line between result sets):

```bash
# Inline SQL.
trace_processor query trace.pftrace "SELECT ts, dur, name FROM slice LIMIT 5"

# From a file (or `-f -` for stdin) — the natural form for scripts.
trace_processor query -f queries.sql trace.pftrace
```

The trace argument can also be an `http(s)://` URL or a Perfetto UI share
link (`https://ui.perfetto.dev/#!/?s=<hash>`); the trace is downloaded and
cached locally under `~/.cache/perfetto/`.

## Iterate without re-parsing: sessions

Parsing the trace is the expensive part — tens of seconds for large
traces — and a plain `query` invocation pays it every time. When you'll
run more than one query against the same trace, load it once into a named
background **session** and point each invocation at it with `--remote`:

```bash
# 1. Load the trace into a background session (once per trace).
trace_processor server unix --name mysession --daemonize trace.pftrace

# 2. Query the warm session: no trace path, no reparse.
trace_processor query --remote mysession \
  "SELECT ts, dur, name FROM slice LIMIT 10"

# 3. Stop the session when you're done with the trace.
trace_processor server kill mysession
```

Session state persists across `--remote` invocations: a
`CREATE PERFETTO TABLE` or `INCLUDE PERFETTO MODULE` from one call is
visible to the next, exactly as within a single interactive shell — so
materializing intermediate results pays off across calls. Idle sessions
are reaped automatically after 30 minutes.

Two things to know:

- Flags that configure trace loading (`--full-sort`,
  `--add-sql-package`, ...) belong on the `server unix` invocation;
  `query --remote` rejects them.
- `--remote` works with `interactive` and `summarize` too, so you can
  drop into a REPL on — or summarize — an already-warm session.

Session naming, socket paths and idle-timeout tuning:
[reference](/docs/analysis/trace-processor.md#subcommand-server).

## Merge traces

To analyze several trace files as one (e.g. traces from two devices, or a
system trace plus an in-process trace), pass a zip or concatenation of
them — for the common case no configuration is needed:

```bash
zip traces.zip trace1.pftrace trace2.pftrace
trace_processor query traces.zip "SELECT count(*) FROM slice"

# Or concatenate protobuf traces directly.
cat trace1.pftrace trace2.pftrace > merged.pftrace
```

When you need control over how the traces combine — keeping devices'
data separate, aligning unsynchronized clocks, naming machines — use a
trace manifest: see
[Merging traces with Trace Processor](/docs/analysis/merging-traces.md).

## Export to a SQLite database

To use tools that speak SQLite (or to hand the data to someone without
Perfetto), export every trace processor table to a database file:

```bash
trace_processor export sqlite -o trace.db trace.pftrace
```

## Convert to another trace format

`convert` wraps the traceconv tool to translate a Perfetto trace into
other formats, e.g. Chrome JSON (loadable in chrome://tracing or other
Catapult tooling) or pprof:

```bash
trace_processor convert json trace.pftrace trace.json
trace_processor convert text trace.pftrace trace.txt
```

Run `trace_processor convert --help` for the full format list, and see
[Converting from Perfetto](/docs/quickstart/traceconv.md) for more on the
underlying traceconv tool.

## Next steps

- Writing the queries themselves:
  [Getting started with PerfettoSQL](/docs/analysis/perfetto-sql-getting-started.md).
- Automating analysis across many traces from Python:
  [Batch Trace Processor](/docs/analysis/batch-trace-processor.md).
- Every subcommand and flag:
  [Trace Processor reference](/docs/analysis/trace-processor.md).
