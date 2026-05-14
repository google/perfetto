# Distributing Perfetto skills to OSS coding agents

**Authors:** @LalitMaganti

**Status:** Draft

## Problem

[RFC-0025](./0025-ai-in-perfetto.md) section 3 sets out the broad direction
for AI integration outside the Perfetto UI: ship Perfetto-flavoured
[skills](https://agentskills.io/home) so that any OSS coding agent
(Claude Code, Gemini CLI, OpenAI Codex, OpenCode) can drive
`trace_processor` to load, query and reason about traces. The seed commit
[39c023b](https://github.com/google/perfetto/commit/39c023b2575616e2bb81c7357cf670f89780404e)
landed the first three skills in `ai/skills/` and established the
authoring conventions.

This RFC pins down the packaging, distribution and update story.

Today, a user who wants Perfetto's skills inside their agent has to clone
`google/perfetto`, install `trace_processor` separately, and copy
`ai/skills/*` into the right location for each agent. They then have to
remember to repeat that work whenever we ship changes. Concrete
problems:

1. There is no first-class install path for any of the major agents.
2. Skills and the `trace_processor` binary are acquired through
   different channels, so users have to do two installs and keep both
   versions in sync themselves.
3. Once installed, skills snapshot at copy time and rot as
   `trace_processor` evolves.
4. A user who has never heard of "Perfetto skills" has no entry point:
   no marketplace listing, no docs page, no install snippet to paste.

This RFC proposes a single packaging mechanism, tied to the existing
Perfetto release process, that addresses all four for the four agents
that matter today while leaving room for the long tail.

## Decision

Pending. The body proposes the following:

* Source of truth lives in `main`, under `ai/skills/` and
  `ai/extensions/<agent>/`.
* The Perfetto release pipeline produces a dedicated `ext/agents`
  branch on each release. The branch carries a pristine, root-level
  layout that every supported agent's install command can fetch
  directly via git ref pinning. OpenCode reads the same content via
  its `skills.urls` config.
* The branch bundles the `trace_processor` Python wrapper
  (`tools/trace_processor`) so users do not need a separate
  `trace_processor` install step.
* Four officially supported install lines, one per agent, documented
  on the Perfetto docs site under an AI integration page.
* A fallback install script lives alongside our other release scripts
  and copies the bundled skills and the `trace_processor` script into
  a user-chosen path. This is the install path for any agent we do not
  officially support (Cursor, Continue, homegrown agents).
* The existing `trace_processor` CLI plus skills cover the workflow.
  We do not ship an MCP server or a binary plugin, and we do not add
  an `install-skills` subcommand to `trace_processor`.
* Slash commands per agent are deferred. v1 is skills-only.
* Marketplace submissions wait until the integration has been stable
  through at least one release cycle.

## Design

### Source-of-truth layout in `main`

Skills are already at `ai/skills/`. We add per-agent extension manifests
alongside, in their own subdirectories:

```text
ai/
├── skills/                                # canonical skill content
│   ├── perfetto-infra-querying-traces/SKILL.md
│   ├── perfetto-infra-getting-trace-processor/SKILL.md
│   ├── perfetto-workflow-android-heap-dump/SKILL.md
│   └── ...
└── extensions/
    ├── claude-code/
    │   ├── plugin.json                    # packaged into .claude-plugin/
    │   └── marketplace.json               # packaged into .claude-plugin/
    ├── gemini-cli/
    │   └── gemini-extension.json          # at branch root
    └── codex/
        ├── plugin.json                    # packaged into .codex-plugin/
        └── marketplace.json               # packaged into .agents/plugins/
```

Each manifest holds the minimum the agent's loader requires: name,
version, the list of skills, and (when we choose to ship them later)
slash commands. There is no per-agent skill content. All four agents
use the agentskills.io `SKILL.md` format, so a single canonical
`skills/` tree serves all of them.

A skill's SKILL.md frontmatter can declare which install targets it
applies to. The release pipeline reads this and includes or excludes
the skill per target. The motivating case is
`perfetto-infra-getting-trace-processor`: it is needed for the
fallback installer (where users may want to set up `trace_processor`
manually) but redundant for the native extensions (which bundle the
script).

### The `ext/agents` release branch

`ext/agents` is produced by the Perfetto release pipeline alongside
the `trace_processor` binary release. Each commit to it corresponds
1:1 with a Perfetto release tag, and the manifest `version` fields all
carry that tag. Treat it as a release artefact, not as a working
branch.

Its root is shaped to satisfy every agent's discovery contract at the
same time:

```text
ext/agents (branch root)
├── gemini-extension.json                  # Gemini reads from root
├── .claude-plugin/
│   ├── plugin.json
│   └── marketplace.json                   # Claude marketplace entrypoint
├── .codex-plugin/
│   └── plugin.json
├── .agents/plugins/
│   └── marketplace.json                   # Codex marketplace entrypoint
├── bin/
│   └── trace_processor                    # bundled Python wrapper
└── skills/                                # shared, agentskills.io layout
    ├── index.json                         # OpenCode skills.urls manifest
    ├── perfetto-infra-querying-traces/SKILL.md
    ├── perfetto-workflow-android-heap-dump/SKILL.md
    └── ...
```

The branch carries only the extension surface. There is no `src/`,
`docs/` or `tools/` content. A user installing the extension pulls a
few tens of kilobytes rather than the full `google/perfetto` tree.

The four manifest filenames are namespace-distinct, so each tool only
sees its own. Skills live in one shared `skills/` tree because all four
tools agree on `<root>/skills/<slug>/SKILL.md` and respect the
agentskills.io frontmatter; nothing is duplicated.

### Coupling to the release process

`ext/agents` is part of the release artefact set, not built from `main`
directly. The whole point of this coupling is that the
`trace_processor` binary, the bundled wrapper script and the skills
move together: when users update one, they update everything. Doc and
skill state on `main` does not surprise installed users mid-cycle.

The release pipeline assembles the branch from `main` at the release
tag, copies in the manifests, skills (filtered by target), and the
`trace_processor` wrapper, sets every manifest's `version` field to
the release tag, and commits. A separate linter step in regular `main`
CI validates each manifest against its tool's schema so we catch
broken JSON before it reaches the release pipeline.

Every commit on `ext/agents` records the SHA of the `main` commit it
was built from, in a small metadata file at the branch root. The
fallback install script is regenerated at the same release tag and
embeds that same SHA. When the script runs, it walks `ext/agents`
history (via a single GitHub API call) to find the commit whose
recorded SHA matches the embedded one, and downloads that commit's
tarball. This makes installs deterministic across release boundaries:
a user who saves the script locally and reruns it later gets the same
bundle they would have gotten on the day it was published, even if
newer releases have landed in the meantime.

### Per-agent install commands

These five lines are the entire user-facing install surface:

| Tool                                 | Install command                                                                                              |
| :----------------------------------- | :----------------------------------------------------------------------------------------------------------- |
| Claude Code                          | `/plugin marketplace add google/perfetto@ext/agents`                                                         |
| Gemini CLI                           | `gemini extensions install https://github.com/google/perfetto --ref ext/agents`                              |
| Codex                                | `codex plugin marketplace add google/perfetto --ref ext/agents`                                              |
| OpenCode                             | add `"skills": { "urls": ["https://raw.githubusercontent.com/google/perfetto/ext/agents/skills/index.json"] }` to `opencode.json` |
| Other agents (Cursor, Continue, ...) | `curl -fsSL https://get.perfetto.dev/agents-install.sh \| bash -s -- --target <path>`                        |

Each tool's native update path applies for the first four. Claude
`/plugin update`, Gemini `extensions update`, the Codex marketplace
refresh, OpenCode's startup URL re-fetch. The release pipeline pushes
to `ext/agents`; users get the changes through the channel they
already know. The fallback script's update path is to re-run the same
command, which overwrites the target directory.

### Bundling `trace_processor`

The release pipeline vendors the auto-generated
`tools/trace_processor` Python wrapper into `ext/agents/bin/`. Users
who install the extension also get the script, so there is no
separate `trace_processor` install step.

The exact mechanism each native extension uses to expose the wrapper
to its skills (relative path from plugin root, declared `bin` in the
manifest, install-time `PATH` entry) is settled per-agent during
manifest authoring.

### Fallback installer

The installer script lives in `tools/` in `main`, alongside our other
end-user scripts, and is served from `get.perfetto.dev` the same way.
It downloads the `ext/agents` tarball at the latest release tag (or
the ref passed via `--ref`), extracts `skills/` and
`bin/trace_processor` into the path passed via `--target`, and prints
a `PATH` hint.

The script is the install path for any agent without a native
extension. It is also the recommended path for users who want a single
on-disk source of truth regardless of agent, including OpenCode users
who want the bundled wrapper rather than installing `trace_processor`
separately (`--target ~/.config/opencode/`).

### Extension store / marketplace submissions

Each of the three plugin-style agents has, or is in the process of
establishing, a public extension index (Gemini CLI's
[extensions index](https://github.com/google-gemini/gemini-cli/tree/main/extensions),
Claude Code's `code.claude.com/plugins`, the upcoming Codex public
index). We submit Perfetto's extension to each once it has been live
in `ext/agents` for at least one release cycle. The submission for
each is a small, tool-specific PR; it is not on the critical path for
the docs page or for v1 install support.

OpenCode has no marketplace surface (plugins are discovered via npm),
so no submission is required.

### Documentation

A new page on the Perfetto docs site carries the five install lines,
a one-line summary of each shipped skill, and a pointer to
`ai/skills/README.md` for teams who want to author or contribute their
own. It also documents the project-scope variant for each install
path, for teams who want to check skills into a shared repo
(`.claude/skills/`, `.opencode/skills/`, etc.) so every developer on
the team gets the same baseline.

The page links back to this RFC and to RFC-0025.

### Out of scope

Several adjacent topics come up in the same conversation but belong in
their own RFCs or follow-ups:

* Long-running session hardening in `trace_processor` (idle-timeout,
  optional parent-PID watch, stdin-EOF detection). The querying skill
  already teaches the `--httpd` background pattern and agents handle
  the process lifecycle through their existing background-shell
  tooling. Cleanup robustness is a `trace_processor` work-item, not a
  packaging one.
* Team-authored skills via extension servers. RFC-0025 §4 covers
  this. The packaging mechanism here would extend naturally because a
  team's extension server can publish its own `index.json` and a
  team's local config can list multiple `skills.urls`. The server-side
  API is separate work.
* Slash commands per agent (`/perfetto-load`,
  `/perfetto-investigate-jank`, ...). Cheap, discoverable, easy to
  add later once we know which workflows users repeatedly type in
  natural language. Skills-only in v1.
* A first-class OpenCode npm plugin. Useful only if we want a custom
  `perfetto_query` tool or auto-spawn hooks. Deferred until there is
  evidence skills alone are insufficient.
* An MCP server. See Alternatives.

## Alternatives considered

### A. `trace_processor ai install-skills` subcommand

Bundle the skills into the binary and ship a subcommand that copies
them into the user's agent skill directories.

Pros: Works fully offline. Skill version equals the binary version.

Cons: Snapshots at install time; users have to re-run after every
release. Encodes the agent skill-directory layout into
`trace_processor`, which leaks packaging concerns into the analysis
engine. Each new agent we want to support requires a code change.

### B. Subdirectory installs (no branch)

Use Codex's `--sparse` and Claude's `git-subdir` source format to
install directly out of `main`'s `ai/extensions/<tool>/`
subdirectories.

Pros: No release-pipeline branch building.

Cons: Gemini CLI does not support subdirectory installs for extensions
(only its standalone `gemini skills install` does), so this approach
cannot cover all four tools. Forces every install to clone the full
`google/perfetto` history, which is hundreds of megabytes for a few
KB of extension content. Couples installed state to whatever happens
to be on `main` at the moment of install rather than to a release.

### C. First-class MCP server

Ship a `trace_processor mcp` mode (or a separate `perfetto-mcp`
binary) that exposes the existing RPC as MCP tools, and have each
extension declare it as an MCP server in the agent's config.

Pros: MCP is a recognisable install ritual. Typed tool surface. Server
holds agent state. The server process is cleaned up automatically when
the agent dies (via stdio EOF).

Cons: Buys little over skills plus a CLI binary in practice. The
`trace_processor` CLI is well-shaped, agents construct shell
invocations reliably from a skill, and the long-running `--httpd` mode
already covers iterative querying. An MCP layer is duplicate plumbing.
The cleanup advantage MCP enjoys is a property of stdio child
processes, not of the protocol; we can get the same property for
`trace_processor` via a small `--exit-on-stdin-eof` or
`--idle-timeout` change without adopting MCP.

### D. Custom OpenCode npm plugin

Publish `@perfetto/opencode-plugin` to npm and rely on it for the
OpenCode install path.

Pros: Matches OpenCode's documented plugin install ritual. Lets us
register custom tools and hooks alongside the skills.

Cons: OpenCode's plugin API cannot register skill paths at runtime;
the only first-class skill-distribution mechanism is `skills.urls`,
which we already use. An npm package adds a release artefact and a
publishing pipeline for what would, in v1, be a strictly worse
skill-discovery experience than the URL approach. Worth revisiting if
real users ask for the tool/hook surface.

## Open questions

* OpenCode `skills.urls` cache invalidation behaviour needs a
  prototype. Confirm that pushes to `ext/agents` propagate to users on
  the next session start and that the cache does not pin a stale copy
  indefinitely.
* The exact mechanism each native extension uses to expose the
  bundled `trace_processor` wrapper to skills (relative path from
  plugin root, declared `bin` in the manifest, install-time `PATH`
  entry) needs to be settled per-agent during manifest authoring.
* Claude Code marketplace ref pinning is documented but lacks an
  official non-default-branch example. Prototype the
  `google/perfetto@ext/agents` install upfront, before the release
  pipeline work, to confirm the contract.
