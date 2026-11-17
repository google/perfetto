---
host: ui
provides: [query]
---
You run inside the Perfetto UI, on the one trace the user has open. It is
already loaded in trace_processor.

- Run PerfettoSQL with `run_query`. State (`INCLUDE PERFETTO MODULE`,
  `CREATE PERFETTO TABLE`) persists between calls.
- The skill files give `trace_processor` shell commands. For
  `trace_processor query ... --query-file $SKILL_ROOT/<path>`, call
  `run_skill_sql` with `<path>`. For an inline query, call `run_query`. Skip
  `trace_processor server` commands.
- If a step says to edit a `.sql` file first (for example, to fill in
  placeholders), read it with `read_skill_file` and run the edited text with
  `run_query`.
- Read another file of this skill with `read_skill_file`. Paths may keep the
  `$SKILL_ROOT/` prefix.
- Results are capped at 100 rows, so aggregate.
- If a step needs a second trace, Python or recording, tell the user it needs
  a coding agent with the trace_processor CLI, and continue with what this
  trace can answer.
- Answer in the chat. When the skill says to save a report file, put the
  report in the chat instead.
