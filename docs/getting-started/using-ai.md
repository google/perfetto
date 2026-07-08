# Using AI with Perfetto

NOTE: **Googlers**: use [go/perfetto-ai-skills](http://go/perfetto-ai-skills)
and
[go/perfetto-ai-skills-android-memory](http://go/perfetto-ai-skills-android-memory)
instead of this page.

Perfetto ships an [agentskills.io](https://agentskills.io) skill for coding
agents. It teaches an agent to invoke `trace_processor`, write PerfettoSQL,
record traces on Android, and follow guided workflows for Android memory and
GPU analysis. Each install bundles a `trace_processor` wrapper, so no separate
binary is needed.

The design is described in
[RFC-0025](https://github.com/google/perfetto/discussions/5763) and
[RFC-0026](https://github.com/google/perfetto/discussions/5892).

## Install

| Agent | Install |
| ----- | ------- |
| Claude Code | `/plugin marketplace add google/perfetto@ai-agents` |
| Codex | `codex plugin marketplace add google/perfetto --ref ai-agents` |
| OpenCode | Add to `opencode.json`: `"skills": { "urls": ["https://raw.githubusercontent.com/google/perfetto/ai-agents/plugins/perfetto/skills"] }` |
| Other (Antigravity, Cursor, ...) | Use the fallback installer (below) |

For any other agent, use the fallback installer (any platform with Python 3):

```bash
# macOS / Linux
curl -fsSL https://get.perfetto.dev/agents-install | python3 - --target <path>
```

```powershell
# Windows (use curl.exe, not the PowerShell curl alias)
curl.exe -fsSL https://get.perfetto.dev/agents-install | python - --target <path>
```

Pass `--agent <claude|codex|opencode|antigravity|pi>` instead of `--target` to
install into that agent's default directory.

To share the setup with your team, point `--target` at a per-agent directory
in your repo (for example `.claude/skills/`) and commit the result.

## Ad-hoc trace analysis

Mention a trace file and ask your question; the agent loads the trace,
discovers the schema, and writes the PerfettoSQL for you.

```
> Load ~/traces/startup.pftrace and tell me which threads used the most CPU
  in the first two seconds.

> Find the top causes of uninterruptible sleep for com.example.myapp in
  trace.pftrace.
```

For Android-specific workflows (memory leak debugging, fleet-wide heap dump
clustering, trace recording), see
[Using AI in the Android cookbook](android-trace-analysis.md#using-ai).

## Debugging GPU performance

Guided workflows answering "is this workload GPU-bound or host-bound?", then
drilling into whichever side is the problem. Deepest counter support is
NVIDIA/CUDA today.

```
> Is this workload GPU-bound or host-bound? The trace is at
  ~/traces/game.pftrace.

> The GPU looks busy but the workload is slow. Was the clock throttled or
  slow to ramp in gpu.pftrace?

> Which kernels dominate this CUDA trace, and are they compute-bound or
  memory-bound?
```

The agent inventories the GPUs, splits the timeline into busy vs idle time
(attributing idle gaps to host-side causes), checks for DVFS ramp or thermal
throttling, and for compute workloads classifies kernels against the
hardware's compute and memory ceilings.

## Contributing

To author or modify a skill, see
[`ai/skills/README.md`](https://github.com/google/perfetto/blob/main/ai/skills/README.md).
