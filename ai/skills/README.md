# Perfetto Skills

This directory holds a single **skill**: model-agnostic instructions
that teach an AI agent how to do something useful with Perfetto. A
skill is how Perfetto and the teams that use it encode the knowledge
an expert would share when sitting next to a colleague — what to look
at, which tables to query, what good queries look like, and how to
interpret the results.

The format follows the [Agent Skills](https://agentskills.io)
convention: a skill is a directory containing a `SKILL.md` with YAML
frontmatter (`name`, `description`) and a markdown body. Any tool that
implements the convention — Claude Code, Gemini CLI, OpenAI Codex —
can load it.

This is part of the ecosystem described in
[RFC-0025: AI in Perfetto](https://github.com/google/perfetto/discussions/5763)
and [RFC-0026](https://github.com/google/perfetto/discussions/5892).

## One skill, a router, and reusable files

Everything Perfetto ships is consolidated into **one** skill,
`ai/skills/perfetto/`. Its entry point is a lean router; the actual
knowledge lives in reference and workflow files the router dispatches
to and loads on demand. This keeps a single, broad `description` in
the agent's context budget instead of many sibling skills competing to
match, and lets each piece be loaded only when the task needs it.

```
ai/skills/perfetto/
├── SKILL-template.md            # the router (see below — NOT named SKILL.md)
├── EVAL.txtpb                   # one eval suite for the whole skill
├── infra-references/
│   └── querying.md              # how to run trace_processor + PerfettoSQL
├── environment-references/
│   ├── setup-bundled.md         # setup when trace_processor is plugin-bundled
│   └── setup-standalone.md      # setup when the agent must fetch the binary
└── workflows/
    └── android_memory/
        ├── heap_dump.md
        ├── heap_dump_cluster.md
        ├── heap_dump_caching_optimizer.md
        └── scripts/             # SQL/Python shipped with these workflows
```

Three kinds of file:

- **`workflows/<domain>/*.md`** — entry points the router dispatches
  *to*: domain-specific guided investigations (a heap dump on Android,
  jank on Chrome, …). Group related workflows in a `<domain>/`
  subfolder. A workflow is self-contained — it carries its own queries
  and any helper scripts under a sibling `scripts/` dir.
- **`infra-references/*.md`** — domain-agnostic mechanics a workflow
  (or an ad-hoc request) pulls *in*: how to query a trace, etc.
- **`environment-references/*.md`** — environment-specific setup. The
  one piece of per-install variance lives here (see below).

## The source tree is a build input, not a drop-in

Unlike a normal Agent Skill, this tree is **not** directly loadable.
Two source-only conventions mean it has to pass through the bundler
(`tools/release/build_ai_agents.py`) before any agent can load it:

1. **`SKILL-template.md`, not `SKILL.md`.** The router is named so a
   discovery layer scanning for `SKILL.md` will not pick up the
   unassembled source tree. The bundler renames it to `SKILL.md`.
2. **Two `setup-*` variants, no `setup.md`.** The router always links
   to `environment-references/setup.md`, but that file does not exist
   in source — only the variants do. The bundler selects one and
   writes it as `setup.md`: `setup-bundled.md` for plugin installs
   (Claude Code, Codex — `trace_processor` ships in the plugin),
   `setup-standalone.md` for fallback installs (OpenCode, Antigravity,
   Pi — the agent fetches the binary itself).

So the only per-environment difference between what each agent gets is
which setup variant became `setup.md`; everything else is identical.
See [`ai/extensions/README.md`](../extensions/README.md) for how the
assembled bundle reaches end users.

## Reference other files by path *relative to the current file*

A workflow that needs the querying reference links to it relative to
its own location, e.g. from `workflows/android_memory/heap_dump.md`:

```markdown
follow `../../infra-references/querying.md` first, then come back here.
```

Not `infra-references/querying.md` (skill-root-relative). The router
sits at the skill root, so its links have no `../`. Helper scripts are
referenced relative to the workflow too, e.g. `scripts/cluster_paths.py`.

This is what lets the files be relocated or repackaged without the
links breaking, and it is verified at build time.

## Authoring

- **Portability.** A file will be read by tools without access to this
  checkout. Never use repo-relative paths like `src/trace_processor/...`
  or refer to `tools/...` scripts; assume the reader only has
  `trace_processor` and a trace. Link to absolute URLs on
  [perfetto.dev/docs](https://perfetto.dev/docs).
- **The router (`SKILL-template.md`)** stays minimal: match broad in
  its `description`, then route. When you add a workflow, add one row
  to its table. Keep it short.
- **Add a workflow** as `workflows/<domain>/<name>.md`, with any
  scripts in a sibling `scripts/`. Write the body in the imperative,
  like a runbook. Pull in `../../infra-references/querying.md` rather
  than re-explaining how to query.
- **Add eval cases** to the single top-level `EVAL.txtpb`, grouped
  under the file they exercise.
- **Test against a real trace** before checking in. Files that have
  never been run end up with broken syntax and wrong column names.
