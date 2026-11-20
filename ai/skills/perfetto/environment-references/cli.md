---
provides: [query, multi_trace, python, record, write_file]
description: >-
  Analyze or record Perfetto traces to debug performance problems: slow app
  startup, jank and dropped frames, ANRs, high CPU, battery drain, memory
  growth, low-memory kills, and Java/native heap leaks, GPU utilisation,
  scheduling latency. Use for any trace file (.pftrace, .perfetto-trace,
  .pb, systrace, ftrace, Chrome JSON) from Android, Linux, Chrome or
  CUDA/GPU workloads, whether or not the user says "Perfetto". Bundles
  trace_processor (PerfettoSQL) plus guided workflows for Android memory
  and GPU analysis.
---
Do this once per session. Set `$SKILL_ROOT` to the absolute path of the
directory containing this `SKILL.md`; every path below is written relative
to it. Then put the bundled `trace_processor` on the `PATH` and check it
runs:

```sh
export SKILL_ROOT="/absolute/path/to/skills/perfetto"   # dir holding SKILL.md
chmod +x "$SKILL_ROOT/bin/trace_processor"
export PATH="$SKILL_ROOT/bin:$PATH"
trace_processor --version
```

This skill ships `trace_processor` and tells you how to drive it. The
first call downloads the native binary for this platform and caches it;
later calls are instant. Do not download `trace_processor` separately.
On Windows invoke it as `python "$SKILL_ROOT/bin/trace_processor" ...`.

### `query`: run SQL, or a `.sql` file of this skill, on a loaded trace

Parsing is the slow part (seconds to minutes for large traces), so load the
trace into a named background session and run every query against it with
`--remote`:

```sh
trace_processor server unix --name mysession --daemonize TRACE_FILE
trace_processor query --remote mysession "SELECT ts, dur, name FROM slice WHERE dur > 5e8 LIMIT 5"
trace_processor query --remote mysession -f queries.sql   # long SQL from a file
trace_processor server kill mysession                     # when completely done
```

When a file says to run a `.sql` file of this skill, run it with
`trace_processor query --remote mysession --query-file $SKILL_ROOT/<path>`.

- `server unix --name <name>` fails if a session with that name is still
  running, and queries then go to the old trace. Before you load a trace, stop
  any old session with the same name:
  `trace_processor server kill <name> 2>/dev/null`.
- Pass SQL that contains `$` (such as the inner class name `Foo$Bar`) with
  `-f - << 'EOF'`, so that the shell does not expand it.
- To run a `.sql` file of this skill with a placeholder filled in, pipe it
  through `sed` into `-f -`, in single quotes so that the shell does not expand
  `$`: `sed 's/<class_name>/Foo$Bar/' $SKILL_ROOT/<path> | trace_processor
  query --remote mysession -f -`.
- Session state persists between calls: a `CREATE PERFETTO TABLE` or
  `INCLUDE PERFETTO MODULE` done once is available in the next call.
- Several `;`-separated statements per call are fine. Each result set is
  printed as CSV, separated by a blank line.
- Trace-loading flags (`--full-sort`, `--add-sql-package`, ...) go on the
  `server unix` invocation, not on `query --remote`.
- `TRACE_FILE` may be a local path, an `http(s)://` URL or a Perfetto UI
  share link; gzipped traces load directly.
- Idle sessions are reaped after 30 minutes; kill yours when done.
- `trace_processor query TRACE_FILE "..."` without a session re-parses on
  every call. Use it only for a single quick question.

### `multi_trace`: hold two or more traces loaded at once

Load each trace into its own named session, with the name that the workflow
gives it, such as `before_tp` and `after_tp`, and pass the matching name to each
query.
To load them at the same time, start each `server unix ... --daemonize` in the
background with `&`, then `wait`.

### `python`: run a Python script of this skill

Run it with `python3 $SKILL_ROOT/<path>`.

### `record`: record a new trace from a device or host

Follow the recording guide that the router links for the device.

### `write_file`: write a report to the user's working directory

For any investigation that took more than a handful of queries, or that
followed a workflow, also save a markdown report in the working directory
(default `perfetto_analysis_report.md`, or where the user asked) so a
human can follow what was done without reading the whole session. It
should contain the question, the trace file(s), the findings with
concrete numbers, the validated queries so the analysis can be re-run,
and open questions or next steps. Mention its path in your final message.
A single quick question answered by one or two queries does not need a
file.
