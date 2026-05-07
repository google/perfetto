# AI in Perfetto

This page is the landing for AI-related functionality in Perfetto.
Today it all revolves around a bundled set of [Agent
Skills](https://agentskills.io) that teach AI coding agents (Claude
Code, Gemini CLI, OpenAI Codex, …) how to work with Perfetto traces.
More AI features will land over time — see
[RFC-0025: AI in Perfetto](https://github.com/google/perfetto/discussions/5763)
for the longer-term direction.

The skills themselves live in-tree at
[`ai/skills/`](https://github.com/google/perfetto/tree/main/ai/skills);
authoring conventions are in
[`ai/skills/README.md`](https://github.com/google/perfetto/blob/main/ai/skills/README.md).

## How to install Perfetto's skills into your AI coding agent

You will need the `trace_processor` binary
([download](https://get.perfetto.dev/trace_processor)) and one of the
supported agents installed locally.

```bash
trace_processor ai install-skills claudecode  # → ~/.claude/skills/
trace_processor ai install-skills geminicli   # → ~/.gemini/skills/
trace_processor ai install-skills codex       # → ~/.agents/skills/
```

The agent picks the skills up on next launch.

## How to inspect or filter the bundled skills

```bash
trace_processor ai list-skills
trace_processor ai search-skills 'heap dump'
```

`--include` and `--exclude` take fnmatch globs against the skill's
slug (e.g. `perfetto-infra-querying-traces`,
`perfetto-workflow-android-heap-dump`). Both are repeatable; `*`
matches across any character, so `perfetto-infra-*` matches the entire
infra category.

```bash
trace_processor ai install-skills claudecode --include 'perfetto-infra-*'
trace_processor ai install-skills claudecode --exclude 'perfetto-workflow-*'
trace_processor ai install-skills claudecode --dest /tmp/x --dry-run
```
