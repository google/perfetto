# Using AI with Perfetto

This page is the starting point for using AI tooling with Perfetto. Today that
means installing Perfetto's [agentskills.io](https://agentskills.io) skills into
your coding agent so it can load, query, and reason about traces; over time it
will grow to cover other AI-assisted workflows (for example in the Perfetto UI).

The skills teach an agent how to invoke `trace_processor`, write PerfettoSQL,
and follow guided analysis workflows. Each install also bundles a
`trace_processor` wrapper, so the agent has a working binary with no extra
setup.

The design behind this work is described in
[RFC-0025](https://github.com/google/perfetto/blob/main/rfcs/0025-ai-in-perfetto.md)
and
[RFC-0026](https://github.com/google/perfetto/discussions/5892).

## Install into your coding agent

Pick the row for your agent and run the command:

| Agent | Install |
| ----- | ------- |
| Claude Code | `/plugin marketplace add google/perfetto@ai-agents` |
| Codex | `codex plugin marketplace add google/perfetto --ref ai-agents` |
| OpenCode | Add to `opencode.json`: `"skills": { "urls": ["https://raw.githubusercontent.com/google/perfetto/ai-agents/skills/index.json"] }` |
| Antigravity | Use the fallback installer (below) |
| Other (Cursor, Continue, ...) | Use the fallback installer (below) |

### Fallback installer

For any agent without a native plugin command, use the fallback installer. It
works on any platform with Python 3:

```bash
# macOS / Linux
curl -fsSL https://get.perfetto.dev/agents-install | python3 - --target <path>
```

```powershell
# Windows (use curl.exe, not the PowerShell curl alias)
curl.exe -fsSL https://get.perfetto.dev/agents-install | python - --target <path>
```

Pass `--agent <claude|codex|opencode|antigravity|pi>` instead of `--target` to
install into that agent's default directory. The installer also bundles
`trace_processor`, so there is no separate binary to install.

## Share the setup with your team

To give everyone on a project the same baseline, check the install into a shared
repository directory. Most agents look for a per-agent directory in the repo
root (for example `.claude/skills/` for Claude Code or `.opencode/skills/` for
OpenCode); point the fallback installer's `--target` at that directory and commit
the result:

```bash
curl -fsSL https://get.perfetto.dev/agents-install | python3 - --target .claude/skills
```

Once committed, the setup travels with the repo and needs no per-developer
install step.

## What gets installed

- **perfetto-infra-querying-traces**: Load a trace, run a PerfettoSQL query, and
  discover the available tables, views, columns, and stdlib modules.
- **perfetto-infra-getting-trace-processor**: How to fetch the `trace_processor`
  binary and the `perfetto` Python client when it is not already bundled.
- **perfetto-infra-trace-processor-bundled**: Points the agent at the
  `trace_processor` binary that ships with the install.
- **perfetto-workflow-android-heap-dump**: A guided workflow for investigating an
  Android Java heap graph (heap dump) to find leaks and understand what is
  retaining memory.
- **perfetto-workflow-android-heap-dump-cluster**: A workflow for clustering
  multiple Android heap dumps for a process to identify common leaks.

## Contributing

To author a new skill or modify an existing one, see
[`ai/skills/README.md`](https://github.com/google/perfetto/blob/main/ai/skills/README.md)
in the Perfetto repository.
