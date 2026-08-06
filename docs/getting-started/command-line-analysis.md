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

# From a file (`-f -` for stdin): the natural form for scripts.
trace_processor query -f queries.sql trace.pftrace
```

The trace argument can also be an `http(s)://` URL or a Perfetto UI share
link (`https://ui.perfetto.dev/#!/?s=<hash>`); the trace is downloaded and
cached locally under `~/.cache/perfetto/`.

## Iterate without re-parsing: sessions

Parsing the trace is the expensive part (tens of seconds for large
traces), and a plain `query` invocation pays it every time. When you'll
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
visible to the next, exactly as within a single interactive shell, so
materializing intermediate results pays off across calls. Idle sessions
are reaped automatically after 30 minutes.

Two things to know:

- Flags that configure trace loading (`--full-sort`,
  `--add-sql-package`, ...) belong on the `server unix` invocation;
  `query --remote` rejects them.
- `--remote` works with `interactive` and `summarize` too, so you can
  drop into a REPL on an already-warm session, or summarize it.

Session naming, socket paths and idle-timeout tuning:
[reference](/docs/analysis/trace-processor.md#subcommand-server).

## Merge traces

To analyze several trace files as one (e.g. traces from two devices, or a
system trace plus an in-process trace), pack them into one archive. For
the common case (traces whose clocks already relate), no configuration is
needed:

```bash
trace_processor util merge -o merged.tar trace1.pftrace trace2.pftrace
trace_processor query merged.tar "SELECT count(*) FROM slice"
```

`util merge` writes a TAR that Trace Processor opens as a single merged
trace, and dry-runs the result to warn if the traces would not merge
cleanly (`--strict` makes that a hard error, handy in CI). Any ZIP or TAR
of trace files opens the same way, so without a `trace_processor`
dependency you can pack them yourself:
`tar cf merged.tar trace1.pftrace trace2.pftrace`.

Do not merge by concatenating the files with `cat`; that is not a merge,
see [Trace merging](/docs/concepts/merging-traces.md).

When you need control over how the traces combine (keeping devices' data
separate, aligning unsynchronized clocks, naming machines), pass a trace
manifest to `util merge` (`--manifest manifest.json`) or tar it into the
archive yourself. See
[Merging traces from the command line](/docs/analysis/merging-traces.md)
for the details, including how to verify a merge placed every event.

## Export trace data

`export` writes the parsed trace data to a file. The format is the first
positional argument, `-o FILE` the output path:

```bash
trace_processor export perfetto -o archive.tar trace.pftrace
trace_processor export arrow_tar -o tables.tar trace.pftrace
trace_processor export sqlite -o trace.db trace.pftrace
```

- **`perfetto`**: a version-coupled archive of the static tables. A fresh
  trace processor instance from the same version can load it back as a trace;
  a different version may load it, but this is not guaranteed. The only
  format that can be reloaded.
- **`arrow_tar`**: one standard [Apache Arrow](https://arrow.apache.org/)
  file per statically registered table, packed in a tar. Stable across trace
  processor versions, for analysis with pandas, Polars or pyarrow. Cannot be
  loaded back into trace processor.
- **`sqlite`**: the statically registered tables plus the trace's views, as
  a SQLite database file that any SQLite tool can open.

All three formats export the statically registered tables; only `sqlite` also
includes views. Runtime tables created during the session (e.g.
`CREATE PERFETTO TABLE`) are not exported. See the
[Trace Processor reference](/docs/analysis/trace-processor.md#subcommand-export)
for the flag and format details.

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
underlying traceconv tool. `convert` translates the trace itself; to dump the
parsed tables instead, see [Export trace data](#export-trace-data) above.

## Next steps

- Writing the queries themselves:
  [Getting started with PerfettoSQL](/docs/analysis/perfetto-sql-getting-started.md).
- Automating analysis across many traces from Python:
  [Batch Trace Processor](/docs/analysis/batch-trace-processor.md).
- Every subcommand and flag:
  [Trace Processor reference](/docs/analysis/trace-processor.md).
