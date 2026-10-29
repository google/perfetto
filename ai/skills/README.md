<div align="center">

<h1>Perfetto agent skill</h1>

<p><strong>Let your coding agent record and analyse Perfetto traces.</strong></p>

<p>
  <a href="#quick-start">Quick start</a> ·
  <a href="#example-prompts">Example prompts</a> ·
  <a href="#what-the-agent-gets">What the agent gets</a> ·
  <a href="CONTRIBUTING.md">Contributing</a>
</p>

</div>

## What is it?

Perfetto ships an [Agent Skills](https://agentskills.io) skill for coding
agents such as Claude Code, Codex, OpenCode, Antigravity and Cursor. Point your
agent at a trace and ask questions in plain English: it loads the trace into
`trace_processor`, writes the PerfettoSQL and reports what it found. It can
also record traces and follow guided workflows for Android memory and GPU
analysis. The skill bundles `trace_processor`, so there is nothing else to
install.

## Quick start

### 1. Install the skill

Pick your agent. The skill is published to the
[`ai-agents`](https://github.com/google/perfetto/tree/ai-agents) branch with
each Perfetto release.

<details open>
<summary>Claude Code</summary>
<p></p>

```
/plugin marketplace add google/perfetto@ai-agents
/plugin install perfetto@perfetto
```

Update through `/plugin`, which pulls the latest `ai-agents` branch.
</details>

<details>
<summary>Codex</summary>
<p></p>

```bash
codex plugin marketplace add google/perfetto --ref ai-agents
```

Then install the `perfetto` plugin from Codex's plugin list.
</details>

<details>
<summary>OpenCode</summary>
<p></p>

Add to `opencode.json`:

```json
"skills": {
  "urls": ["https://raw.githubusercontent.com/google/perfetto/ai-agents/plugins/perfetto/skills"]
}
```

The URL always serves the latest published skill.
</details>

<details>
<summary>Antigravity, Cursor, Pi and any other agent</summary>
<p></p>

Use the installer, which needs Python 3:

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

Re-run the same command to update. Pass `--version vX.Y` to pin a release.
</details>

<details>
<summary>Offline</summary>
<p></p>

Each [GitHub release](https://github.com/google/perfetto/releases) carries a
`perfetto-ai-skill.zip`. Unzip it into your agent's skills directory, for
example `.claude/skills/`. See the
[offline install docs](https://perfetto.dev/docs/getting-started/using-ai#offline-install)
for seeding the `trace_processor` cache on a machine with no network.
</details>

Offline installs, updating and more are covered in the
[docs](https://perfetto.dev/docs/getting-started/using-ai).

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

### Recording

Traces from a connected Android device or the local Linux machine.

```
> Record a 10 second trace of com.example.app starting up on my connected
  device, then tell me what took the longest.
```

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

## What the agent gets

- **`trace_processor`**, bundled inside the skill as a wrapper that
  downloads the matching prebuilt on first use.
- **A router** (`SKILL.md`) whose `description` is what makes the agent
  reach for the skill at all. It names symptoms and file types (slow
  startup, jank, ANRs, memory growth, GPU, `.pftrace`, systrace, Chrome
  JSON) rather than just "Perfetto", because users describe problems,
  not tools. The body is short: setup, then "which of these is your
  situation", each pointing at one file.
- **Workflows** (`workflows/<domain>/*.md`) for investigations we know
  how to do well: Android Java heap dumps, native heap profiles,
  allocation churn, GPU busy/idle decomposition, frequency residency,
  compute kernel analysis. Each is a runbook that runs shipped SQL or
  Python scripts and tells the agent how to read the result.
- **A querying reference** (`infra-references/querying.md`) for
  everything else: how to keep a trace loaded across many queries, how
  to discover tables and the SQL standard library instead of guessing,
  which modules answer the common questions, and a short list of
  PerfettoSQL rules of thumb.
- **Recording guides** for Android and Linux, with example configs, for
  when there is no trace yet.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for how the skill is designed,
how to change it and how it ships.
