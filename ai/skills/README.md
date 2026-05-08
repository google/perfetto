# Perfetto Skills

This directory holds **skills**: self-contained, model-agnostic
instructions that teach an AI agent how to do something useful with
Perfetto. Skills are how Perfetto and the teams that use it encode
the knowledge an expert would share when sitting next to a colleague
— what to look at, which tables to query, what good queries look
like, and how to interpret the results.

The format follows the [Agent Skills](https://agentskills.io)
convention: each skill is a directory containing a `SKILL.md` with
YAML frontmatter (`name`, `description`) and a markdown body. Any
tool that implements the
convention — Claude Code, Gemini CLI, OpenAI Codex — can load them.

This is the **seed** of a much larger skill ecosystem described in
[RFC-0025: AI in Perfetto](https://github.com/google/perfetto/discussions/5763).
External teams will eventually contribute their own skills, either
checked in here or served from extension servers.

## Skills are designed to be copied

A skill is portable. It will be vendored into agent contexts (e.g.
`~/.claude/skills/`), pasted into other repos, and read by tools
running without access to the Perfetto source tree. So:

- **Never use repo-relative paths** like `src/trace_processor/...`
  or `docs/analysis/...` — they don't resolve outside this checkout.
  Link to [perfetto.dev](https://perfetto.dev/docs) instead.
- **Don't refer to "this repo" or `tools/...` scripts** — assume
  the reader only has a Perfetto build (`trace_processor`) and the
  trace.
- **Keep the skill self-sufficient.** If a skill needs a SQL helper
  or example, ship it inside the skill directory next to `SKILL.md`.

## Layout

The directory is **flat**: every skill is a direct child of
`ai/skills/`. This matches the discovery rules of every supported
agent (`<root>/<slug>/SKILL.md`), so the same tree drops into
`~/.claude/skills/`, `~/.gemini/skills/`, or `~/.agents/skills/`
without any flattening step.

```
ai/skills/
├── README.md
├── perfetto-infra-querying-traces/
│   └── SKILL.md
├── perfetto-infra-getting-trace-processor/
│   └── SKILL.md
└── perfetto-workflow-android-heap-dump/
    └── SKILL.md
```

The slug doubles as the taxonomy: `perfetto-` is the vendor prefix
(so Perfetto skills don't collide with anything else the user has
installed), then a category, then any sub-categories, then the
skill name. Two categories today:

- **`perfetto-infra-*`** — domain-agnostic mechanics of working with
  Perfetto: how to query a trace, how to install the binary, etc.
- **`perfetto-workflow-<domain>-*`** — domain-specific
  investigation workflows: how to investigate a heap dump on
  Android, jank on Chrome, etc.

A workflow skill *uses* infra skills — it tells the agent "you'll
need to query the trace; here's the bit specific to this problem."

## Authoring a skill

Create a directory under `ai/skills/` whose name follows the
`perfetto-<category>-<name>` pattern, with a `SKILL.md` inside:

```markdown
---
name: perfetto-infra-querying-traces
description: Use when the user wants to load a Perfetto trace, run
  a SQL query against it, or discover which tables and columns are
  available. Covers trace_processor invocation and the PerfettoSQL
  standard library.
---

# Body of the skill, written for an AI agent to follow.
```

Guidelines:

- **`name`** must equal the directory slug. Agents key off the
  frontmatter name and discovery is a single level deep.
- **`description`** must clearly state *when* the skill should be
  invoked. The agent uses this line to decide whether the skill is
  relevant, so be specific about the trigger conditions, not just
  the topic.
- The body is regular markdown. Write it in the imperative ("run X",
  "check Y"), the same way you would write a runbook.
- **Always link to absolute URLs on
  [perfetto.dev/docs](https://perfetto.dev/docs)**, not
  repo-relative paths.
- **Test your skill against a real trace** before checking it in.
  Skills that have never been run end up with broken syntax and
  wrong column names.
- A skill directory can contain additional files (example scripts,
  reference SQL). Reference them from `SKILL.md` by relative path.
- Keep the body focused. If a skill grows past a couple of pages,
  split it.
