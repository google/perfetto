---
provides: [query]
description: >-
  Analyze a Perfetto trace to debug performance problems: slow app startup,
  jank and dropped frames, ANRs, high CPU, battery drain, memory growth,
  low-memory kills, Java/native heap leaks, GPU utilisation, scheduling
  latency. Includes guided workflows for Android memory and GPU analysis.
---
You run inside the Perfetto UI, on the one trace the user has open. It is
already loaded in trace_processor. Read another file of this skill with
`read_skill_file`; paths may keep the `$SKILL_ROOT/` prefix. If a step needs
a second trace, Python or recording, tell the user it needs a coding agent
with the trace_processor CLI, and continue with what this trace can answer.
Answer in the chat.

### `query`: run SQL, or a `.sql` file of this skill, on a loaded trace

- Run PerfettoSQL with `run_query`. State (`INCLUDE PERFETTO MODULE`,
  `CREATE PERFETTO TABLE`) persists between calls.
- When a file says to run a `.sql` file of this skill, call `run_skill_sql`
  with its path.
- If a step says to edit a `.sql` file first (for example, to fill in
  placeholders), read it with `read_skill_file` and run the edited text with
  `run_query`.
- Results are capped at 100 rows, so aggregate.
