# Cookbook: Using AI with Perfetto

NOTE: **Googlers**: use [go/perfetto-ai-skills](http://go/perfetto-ai-skills)
and
[go/perfetto-ai-skills-android-memory](http://go/perfetto-ai-skills-android-memory)
instead of this page.

The Perfetto AI skill is a [skill](https://agentskills.io) for coding agents
such as Claude Code, Codex, OpenCode, Antigravity and Cursor. Point your agent
at a trace and ask questions in plain English: it loads the trace into
`trace_processor`, writes the PerfettoSQL and reports what it found. It can
also record traces and follow guided workflows for Android memory and GPU
analysis. The skill bundles `trace_processor`, so there is nothing else to
install.

## Quick start

### 1. Install the skill

| Agent | Install |
| ----- | ------- |
| Claude Code | `/plugin marketplace add google/perfetto@ai-agents`, then `/plugin install perfetto@perfetto` |
| Codex | `codex plugin marketplace add google/perfetto --ref ai-agents`, then install `perfetto` from the plugin list |
| OpenCode | Add to `opencode.json`: `"skills": { "urls": ["https://raw.githubusercontent.com/google/perfetto/ai-agents/plugins/perfetto/skills"] }` |
| Other (Antigravity, Cursor, ...) | Use the fallback installer (below) |

For any other agent, use the fallback installer, which needs Python 3:

```bash
# macOS / Linux: into an agent's default directory
curl -fsSL https://get.perfetto.dev/agents-install | python3 - --agent <claude|codex|opencode|antigravity|pi>

# macOS / Linux: into any skills directory, e.g. one committed to your repo
curl -fsSL https://get.perfetto.dev/agents-install | python3 - --target <path>
```

```powershell
# Windows (use curl.exe, not the PowerShell curl alias)
curl.exe -fsSL https://get.perfetto.dev/agents-install | python - --target <path>
```

To share the setup with your team, point `--target` at a per-agent directory
in your repo (for example `.claude/`) and commit the result.

### 2. Ask about a trace

Mention a trace file and ask your question:

```
> Investigate object retention and potential leaks in heap_dump.pftrace.
```

No trace yet? Let the agent record one:

```
> Record a Java heap dump of com.example.app from my connected device and
  find what is keeping memory alive.
```

You do not need to mention the skill: the agent picks it up whenever the
conversation involves a trace or a performance problem. In Claude Code you can
also invoke it explicitly with `/perfetto:perfetto`.

## Example prompts

### Memory leaks and heap dumps

Guided workflows for leaks, OOMs, churn and allocation hot paths.

```
> Investigate object retention and potential leaks in heap_dump.pftrace.

> Why does com.example.app's Java heap keep growing? Compare before.pftrace
  and after.pftrace.

> Analyze native heap allocations and find memory growth hot paths in
  native_heap.pftrace.

> Which code paths allocate the most short-lived objects in
  java_alloc.pftrace?

> Here are 40 heap dumps of com.example.app in ~/dumps/. Cluster them and
  find the common root causes.
```

The agent runs tested SQL to extract dominator paths, native callstacks or
allocation summaries, then searches your workspace for the suspect classes and
points at the files to fix. See
[Using AI in the Android cookbook](android-trace-analysis.md#using-ai) for
more.

### Ad-hoc trace analysis

Any question you could answer with PerfettoSQL.

```
> Load ~/traces/startup.pftrace and tell me which threads used the most CPU
  in the first two seconds.

> Why is app startup slow in startup.pftrace? Break down the main thread.

> Find the top causes of uninterruptible sleep for com.example.myapp in
  trace.pftrace.

> Which frames were janky in jank.pftrace and what was the UI thread doing?
```

The agent discovers the tables and standard library modules it needs rather
than guessing, and keeps the trace loaded across queries.

### Recording

Traces from a connected Android device or the local Linux machine.

```
> Record a 10 second trace of com.example.app starting up on my connected
  device, then tell me what took the longest.
```

The agent picks a config from the bundled examples, records the trace and
then analyses it.

### GPU performance

Is the workload GPU-bound or host-bound, and why. Counter support is deepest
for NVIDIA/CUDA today.

```
> Is this workload GPU-bound or host-bound? The trace is at game.pftrace.

> The GPU looks busy but the workload is slow. Was the clock throttled or
  slow to ramp in gpu.pftrace?

> Which kernels dominate this CUDA trace, and are they compute-bound or
  memory-bound?
```

The agent splits the timeline into busy and idle time, attributes idle gaps to
host-side causes, checks for DVFS ramp or thermal throttling, and for compute
workloads classifies kernels against the hardware's compute and memory
ceilings.

## Offline install {#offline-install}

Machines that can't reach github.com at install time can use the
`perfetto-ai-skill.zip` asset attached to each
[GitHub release](https://github.com/google/perfetto/releases): download it
where you have connectivity, copy it across, and unzip it into your agent's
skills directory (for example `.claude/skills/`). It contains a single
`perfetto/` skill folder with `SKILL.md` inside — no installer needed.

The bundled `bin/trace_processor` wrapper downloads the native
`trace_processor` binary on first use and caches it in
`~/.local/share/perfetto/prebuilts/` under the name
`trace_processor_shell-<first 16 hex chars of its sha256>`. On a fully
offline machine, seed that cache yourself: download your platform's prebuilt
zip from the same release page (for example `linux-amd64.zip`, containing
`trace_processor_shell`), then run:

```sh
mkdir -p ~/.local/share/perfetto/prebuilts
SHA=$(sha256sum trace_processor_shell | cut -c1-16)
cp trace_processor_shell ~/.local/share/perfetto/prebuilts/trace_processor_shell-$SHA
```

The wrapper trusts any file already present under that name, so the binary
must come from the same release. On Windows the cache directory is
`%USERPROFILE%\.local\share\perfetto\prebuilts` and the file is
`trace_processor_shell-<sha256 prefix>.exe`.

## Updating

Updating uses the same mechanism as installing:

| Installed via | Update by |
| ------------- | --------- |
| Claude Code marketplace | Claude Code's normal plugin update flow (`/plugin` → manage/update, which pulls the latest `ai-agents` branch). |
| Codex marketplace | Codex's plugin update mechanism. |
| OpenCode `skills.urls` | Nothing to do — the URL always serves the latest published skill. |
| Fallback installer | Re-run the same `curl ... agents-install` command. It detects the existing install and asks before replacing it (pass `--yes` to skip the prompt). |

New skill versions are published with each Perfetto release. The fallback
installer installs the latest release by default; pass `--version vX.Y` to
pin a specific one.

## Design

The design is described in
[RFC-0025](https://github.com/google/perfetto/discussions/5763) and
[RFC-0026](https://github.com/google/perfetto/discussions/5892).

## Contributing

To author or modify a skill, see
[`ai/skills/CONTRIBUTING.md`](https://github.com/google/perfetto/blob/main/ai/skills/CONTRIBUTING.md).
