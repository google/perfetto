# Subcommand-Based CLI for trace_processor_shell

**Authors:** @lalitm

**Status:** Draft

## Problem

`trace_processor_shell` has grown into a monolithic CLI with ~35 flags spanning
unrelated concerns: HTTP serving, SQL queries, trace summarization, v1 metrics,
SQLite export, and interactive mode.

1. **Discoverability:** `--help` is a wall of text. Related flags are
   categorized in the help output but the invocation syntax offers no structure.

2. **Flag interactions:** Some flag combinations are invalid (e.g., `--summary`
   + `--run-metrics`), some are implicitly composable (e.g., `-q` +
   `--run-metrics` suppresses metrics output), and some silently override each
   other.

3. **Extensibility:** The long-term vision is for `trace_processor_shell` to
   become the equivalent of `tracebox`: a multi-tool binary bundling all trace
   analysis features. Integrating traceconv alone would add another ~12 modes
   to an already crowded flag surface.

4. **Tooling:** Scripts and CI benefit from explicit subcommands with
   well-defined contracts. AI tools benefit from targeted `help <command>`
   instead of ingesting the full flag dump.

## Decision

Add subcommands. The classic flag-based interface stays permanently and
unchanged — it's what most users know, and subcommands strictly scope new
features so the two can coexist without conflicts. All *new* features will
only be exposed through subcommands.

## Design

### Subcommands

```
trace_processor_shell <subcommand> [FLAGS] [trace_file]
```

| Subcommand  | Replaces              | Description                     |
| ----------- | --------------------- | ------------------------------- |
| `query`     | `-q`, `-Q`            | Run SQL queries                 |
| `interactive` | (default interactive) | Interactive SQL shell (default) |
| `server`    | `--httpd`, `--stdiod` | Start RPC server                |
| `summarize` | `--summary`           | Trace summarization             |
| `metrics`   | `--run-metrics`       | v1 metrics (soft-deprecated)    |
| `export`    | `-e`                  | Export to database              |

### Detection and backwards compatibility

Pre-scan `argv` for the first positional word (skipping flags and their
arguments). If it matches a known subcommand name, use the subcommand path.
Otherwise fall through to the classic `ParseCommandLineOptions` path silently.

This is safe because classic invocations only have the trace file as a
positional argument (always last), and trace files don't collide with
subcommand names. In the unlikely event of a collision (file literally named
`query`), detect it and emit a hint suggesting `./query` to disambiguate.

Global flags can appear before or after the subcommand name:

```bash
trace_processor_shell --dev query -f q.sql trace.pb
trace_processor_shell query --dev -f q.sql trace.pb   # equivalent
```

### Per-subcommand flags

**`query`**

SQL can be provided as a positional argument, via `-f FILE`, or piped to stdin:

```
  trace_processor_shell query trace.pb "SELECT ts, dur FROM slice"
  trace_processor_shell query -f query.sql trace.pb
  trace_processor_shell query trace.pb < query.sql
  trace_processor_shell query -f - trace.pb   # explicit stdin

  -f, --query-file FILE  SQL file to execute (use '-' for stdin).
  -i, --interactive      Drop into REPL after query.
  -W, --wide             Double column width.
  -p, --perf-file FILE   Write timing data.
```

**`interactive`**

```
  -W, --wide             Double column width.
```

**`server`** — mode is positional (`server http` or `server stdio`):

```
  trace_processor_shell server <mode> [FLAGS] [trace_file]
  --port PORT            HTTP port (http mode only).
  --ip-address IP        HTTP bind address (http mode only).
  --additional-cors-origins O1,O2,...

  Modes: http, stdio
```

**`summarize`**

```
  --spec PATH            TraceSummarySpec proto path.
  --metrics-v2 IDS       Metric IDs, or "all".
  --metadata-query ID    Metadata query ID.
  --format [text|binary] Output format.
  --post-query FILE      SQL file to run after summarization.
  -i, --interactive      Drop into REPL after.
```

**`metrics`**

```
  --run NAMES            Comma-separated metric names.
  --pre FILE             SQL file before metrics.
  --output [binary|text|json]
  --extension DISK@VIRTUAL
  --post-query FILE      SQL file to run after metrics.
  -i, --interactive      Drop into REPL after.
```

**`export`** — format is positional:

```
  trace_processor_shell export <format> -o FILE trace_file
  Formats: sqlite
```

### Global flags

These configure the TP instance and trace ingestion. They are shared across
all subcommands:

```
--full-sort, --no-ftrace-raw, --crop-track-events,
--analyze-trace-proto-content, --register-files-dir PATH,
--add-sql-package PATH[@PKG], --override-sql-package PATH[@PKG],
--override-stdlib PATH,
--dev, --dev-flag KEY=VALUE, --extra-checks,
--metatrace FILE, --metatrace-buffer-capacity N, --metatrace-categories CATS,
-h/--help, --help-classic, -v/--version
```

### Help system

Three tiers:

**`--help`** shows subcommands with inline examples:

```
Perfetto Trace Processor.
Usage: trace_processor_shell [command] [flags] [trace_file]

If no command is given, opens an interactive SQL shell on the trace file.

Commands:

  query         Run SQL queries against a trace.
                  trace_processor_shell query trace.pb "SELECT ts, dur FROM slice"
                  trace_processor_shell query -f query.sql trace.pb
                  trace_processor_shell query trace.pb < query.sql
                Flags: -f FILE, -i (interactive after), -W (wide)

  interactive   Interactive SQL shell (default).
                  trace_processor_shell trace.pb
                  trace_processor_shell interactive trace.pb
                Flags: -W (wide)

  server        Start an RPC server.
                  trace_processor_shell server http trace.pb
                  trace_processor_shell server http --port 9001 trace.pb
                  trace_processor_shell server stdio
                Modes: http, stdio

  summarize     Run trace summarization.
                  trace_processor_shell summarize --metrics-v2 all --spec spec.textproto trace.pb
                Flags: --spec PATH, --metrics-v2 IDS, --format [text|binary]

  metrics       Run v1 metrics (deprecated).
                  trace_processor_shell metrics --run android_cpu trace.pb
                Flags: --run NAMES, --output [binary|text|json]

  export        Export trace to a database file.
                  trace_processor_shell export sqlite -o out.db trace.pb
                Formats: sqlite

  convert       Convert trace format (planned).
                  trace_processor_shell convert json trace.pb out.json

Global flags (apply to all commands):
  --dev, --full-sort, --no-ftrace-raw, --metatrace FILE, ...
  Run 'trace_processor_shell help <command>' for full flag details.

Previous versions of trace_processor_shell used a flat flag interface
(e.g. -q file.sql, --httpd, --summary, -e output.db). This interface
is fully supported and will remain so permanently. If you have existing
scripts or are following older documentation that uses these flags, they
will continue to work exactly as before.
  Run 'trace_processor_shell --help-classic' to see the flat flag reference.
```

**`help <command>`** shows full details for one subcommand. Example:

```
$ trace_processor_shell help query

Run SQL queries against a trace.

Usage: trace_processor_shell query [flags] <trace_file> [SQL]

SQL can be provided as a positional argument, via -f FILE, or piped to stdin.

Flags:
  -f, --query-file FILE  Read and execute SQL from a file (use '-' for stdin).
  -i, --interactive      Drop into REPL after query.
  -W, --wide             Double column width for output.
  -p, --perf-file FILE   Write timing data to FILE.

Global flags:
  --full-sort            Force full sort.
  --no-ftrace-raw        Skip typed ftrace in raw table.
  --crop-track-events    Ignore track events outside range of interest.
  --dev                  Enable development-only features.
  --dev-flag KEY=VALUE   Set a dev flag (requires --dev).
  --extra-checks         Enable additional SQL validation.
  --add-sql-package PATH[@PKG]
                         Register SQL package directory.
  --metatrace FILE       Write TP metatrace to FILE.
  (... and other global flags)

Examples:
  trace_processor_shell query trace.pb "SELECT ts, dur FROM slice LIMIT 10"
  trace_processor_shell query -f query.sql trace.pb
  trace_processor_shell query trace.pb < query.sql
  trace_processor_shell query -f query.sql -i trace.pb   # interactive after
```

**`--help-classic`** prints the original flat flag-based help (i.e., what
`--help` prints today). This is for users with existing scripts or following
older documentation that uses flags like `-q`, `--httpd`, `--summary`, `-e`,
etc. All of these flags continue to work exactly as before — the classic
interface is permanently supported and will never be removed.

### Backwards compatibility testing

Today there is only one direct CLI test (`--stdiod` in
`test/trace_processor_shell_integrationtest.cc`). Diff tests exercise flags
indirectly but don't validate CLI parsing. A refactor could silently break
classic invocations.

As part of this work, add integration tests covering all classic invocations
and their subcommand equivalents. Tests use `base::Subprocess`, validate exit
codes and spot-check output.

### Future

- Integrate traceconv as `convert` subcommand: `convert json trace.pb out.json`.
- The classic interface stays permanently — no deprecation, no removal.

## Alternatives considered

1. **Keep flat flags** — Doesn't scale to bundling traceconv and future tools.
2. **Separate binaries** — Opposite of the tracebox multi-tool direction.
3. **`--mode=query`** — Unconventional; poor per-subcommand `--help` support.

## Resolved questions

- **Structured queries** stay in `query` (`--structured-spec`, `--structured-id`).
- **Traceconv** becomes `convert` with positional format: `convert json ...`.
