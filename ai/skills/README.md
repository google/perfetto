# The Perfetto agent skill

`ai/skills/perfetto/` is the skill that teaches a coding agent (any
agent that reads the [Agent Skills](https://agentskills.io) format) how
to record and analyse Perfetto traces. It is what `perfetto.dev/docs/getting-started/using-ai`
installs. This file explains what it does, how it is put together and
why; the mechanics of shipping it are at the end.

## What the agent gets

Once installed, the agent has:

- **`trace_processor`**, bundled inside the skill as a wrapper that
  downloads the matching prebuilt on first use. No separate install.
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
  Python scripts and tells the agent how to read the result. They exist
  because a fixed procedure with a script beats open-ended exploration
  on these problems: faster, cheaper and less likely to invent a leak
  that is not there.
- **A querying reference** (`infra-references/querying.md`) for
  everything else: how to keep a trace loaded across many queries, how
  to discover tables and the SQL standard library instead of guessing,
  which modules answer the common questions, and a short list of
  PerfettoSQL rules of thumb.
- **Recording guides** for Android and Linux, with example configs, for
  when there is no trace yet.

## Design choices, and the evidence behind them

The skill was measured with the harness in [`ai/evals/`](../evals/):
the same prompts run with and without it, several times, on two models.
The findings that shaped it (details in
[`ai/evals/REPORT.md`](../evals/REPORT.md)):

- **Capable models already write correct PerfettoSQL.** With only the
  binary on `PATH`, Opus answered nearly every test question correctly.
  The skill's job is therefore not to teach SQL from scratch but to make
  the agent efficient (load once, query many), honest (report numbers
  from the trace, say when the trace does not contain what was asked),
  and reliable on the guided workflows, where a weaker model without the
  skill did invent a memory leak.
- **Every extra instruction has a price.** An earlier version with a
  mandatory setup document, a smoke test, a schema-check checklist and
  an always-on report file made simple questions three times slower for
  the same answer. Keep the router lean, keep references short, and say
  why rather than shouting MUST; agents follow reasons better than
  rules.
- **Warm sessions are the single biggest efficiency win.** Without
  guidance agents re-parse the trace for every query, dozens of times
  per investigation. `querying.md` makes `server unix` plus
  `query --remote` the default.
- **Workflows carry their own scripts.** A workflow that says "run this
  SQL file, then interpret these columns" is repeatable and cheap to
  verify; a workflow that says "explore the heap" is neither.
- **Reports for multi-step work.** Anything that took more than a few
  queries writes `perfetto_analysis_report.md` so a person can follow
  what was done and re-run the validated queries. One-off questions stay
  in chat.

When you change the skill, run the evals. A change that reads well but
costs more or scores lower is not an improvement.

## Layout

```
ai/skills/perfetto/
├── SKILL-template.md            the router (becomes SKILL.md when bundled)
├── infra-references/
│   ├── querying.md              trace_processor sessions, discovery, PerfettoSQL
│   ├── recording_android_traces.md
│   ├── recording_linux_traces.md
│   ├── trace_config_reference.md
│   └── example-configs/
├── environment-references/
│   └── setup.md                 $SKILL_ROOT and the bundled trace_processor
└── workflows/
    ├── android_memory/          heap dumps, native heap, allocations + scripts/
    └── gpu/                     occupancy, frequency, compute kernels + scripts/
```

Paths inside the skill are written as `$SKILL_ROOT/<path>`, where
`$SKILL_ROOT` is the directory holding `SKILL.md`. The skill is loaded
from a plugin directory, not from the user's workspace, so relative
paths would resolve against the wrong place; the router tells the agent
to set `$SKILL_ROOT` once.

## Authoring

- Write for an agent that has only `trace_processor` and a trace: no
  repo paths, no `tools/` scripts. Link to <https://perfetto.dev/docs>.
- Adding a workflow: `workflows/<domain>/<name>.md` in the imperative,
  scripts in a sibling `scripts/`, one new line in the router. Point at
  `querying.md` for how to query rather than repeating it.
- Run every query in a new or changed file against a real trace first.
  Files that were never run ship with wrong column names.
- Add an eval case for any workflow, with ground truth computed by
  `trace_processor_shell`, and check the with/without numbers before
  sending the change.

## How it ships

The tree here is a build input, not a drop-in: `SKILL-template.md` is
named so that skill loaders scanning for `SKILL.md` do not pick up the
unassembled source, and the `trace_processor` wrapper is not checked in
alongside it. `tools/release/build_ai_agents.py` renames the router,
copies in `tools/trace_processor` as `bin/trace_processor`, adds the
per-agent manifests from [`ai/extensions/`](../extensions/README.md) and
writes the result to the `ai-agents` branch at each release. Users
install from that branch; see
[`docs/getting-started/using-ai.md`](../../docs/getting-started/using-ai.md).
To try the local tree, `ai/evals/setup_assets.py` bundles it the same way.
