# Splitting Perfetto skills across hosts

**Authors:** @LalitMaganti

**Status:** Draft

## Problem

`ai/skills/perfetto/` is written for one host: a coding agent with a shell,
Python, a filesystem, and a local `trace_processor` binary. Other hosts now
want to use the same workflows:

| Host | What it can do | How it gets the content today |
| --- | --- | --- |
| Coding agents (`cli`) | Query traces, load multiple traces, run Python, record traces, write files | `ai-agents` release branch, marketplaces, `agents-install` |
| Perfetto UI (`ui`) | Query the single trace open in the UI | Not supported yet |
| google3 | Same as coding agents, with an internal `trace_processor` setup | The coding agent bundle |
| Android CLI | A subset of workflows | Copies selected pieces in by hand |

Host-specific assumptions are spread through the skill files: shell commands,
`$SKILL_ROOT`, Python scripts, loading two traces at once, recording, and
writing report files. A host like the UI cannot run Python, record traces, or
load a second trace; a host like google3 can run all of those, but sets up
`trace_processor` differently.

We want one shared source tree and a script that builds a skill bundle for each
host. Each workflow declares what capabilities it depends on, each host
declares what it provides, and the script bundles only the workflows that host
can run with links that make sense inside that bundle. Adding a host should not
require editing the shared content, and nothing should be copied by hand.

## Decision

Pending

## Design

### Actions

We represent host capabilities as short tags called actions. The initial set:

* `query`: run SQL, or a shipped `.sql` file, on a loaded trace.
* `multi_trace`: hold two or more traces loaded at once.
* `python`: run a shipped Python script.
* `record`: record a new trace from a device or host.
* `write_file`: write a report to the user's working directory.

Actions are open tags, not a fixed list in the generator. They only need to be
granular enough to separate what different hosts can do. If we later add
workflows that drive UI tracks or views directly, those files can depend on a
`ui` action without changing the generator.

Action names only appear in frontmatter. In the body of a workflow, steps are
written in plain prose rather than shell commands or special syntax, and the
agent matches them to the instructions in its host adapter. For example, a
workflow says "run
`$SKILL_ROOT/workflows/android_memory/scripts/triage_dominator_path.sql`"
instead of
`trace_processor query --remote SESSION --query-file $SKILL_ROOT/...`.

### Adapters

Each host has one adapter file in `environment-references/`. Its frontmatter
names the host and lists the actions it provides, and its body tells the agent
how to perform them in that environment.

`environment-references/cli.md` is the coding agent adapter (replacing
`setup.md`):

````markdown
---
host: cli
provides: [query, multi_trace, python, record, write_file]
---
Do this once per session. Set `$SKILL_ROOT` to the absolute path of the
directory containing this `SKILL.md`, and put the bundled `trace_processor` on
the `PATH`:

```sh
export SKILL_ROOT="/absolute/path/to/skills/perfetto"
chmod +x "$SKILL_ROOT/bin/trace_processor"
export PATH="$SKILL_ROOT/bin:$PATH"
trace_processor --version
```
````

`environment-references/ui.md` is the UI adapter:

```markdown
---
host: ui
provides: [query]
---
You run inside the Perfetto UI, on the one trace the user has open. It is
already loaded in trace_processor.

- Run inline PerfettoSQL with `run_query`. State (`INCLUDE PERFETTO MODULE`,
  `CREATE PERFETTO TABLE`) persists between calls.
- Run a shipped `.sql` file (`$SKILL_ROOT/<path>`) with `run_skill_sql`.
- Read another file of this skill with `read_skill_file`.
- Answer in the chat.
```

The generator places the adapter's body into `SKILL-template.md` at
`{{environment_setup}}` when building `SKILL.md`. The adapter is the only file
that differs between hosts.

Adapters don't have to live in this repo. Google-internal, OEM and CI
environments often mandate their own `trace_processor` and setup, and today
`setup.md` can only tell the agent to "prefer that team-specific setup". Such
an environment passes its own adapter file to the generator
(`--adapter <path>`) and gets the shared content with its setup in place of
ours, without forking any content. A custom `trace_processor` build can also
carry its adapter through the platform interface so that installing from that
binary picks it up.

### Files declare what they need

Each content file lists in frontmatter every action it needs:

```yaml
---
requires: [query, multi_trace, python]
---
```

The whole file is the unit of inclusion. A host gets a file only if its
adapter provides everything the file requires. Frontmatter is stripped in the
output, and nothing is trimmed from inside a file.

If part of a workflow should reach a host that lacks an action, it goes in its
own file. For example, in `system_trace_memory.md`, the "Recording the missing
signals" section moves to `recording_memory_signals.md` (`requires: [record]`)
and the two-trace comparison section moves to `system_trace_memory_compare.md`
(`requires: [query, multi_trace]`). `system_trace_memory.md` only needs `query`,
so the UI keeps the single-trace workflow.

Features a host can't support are dropped, not reimplemented. A host gains a
workflow by implementing the action it needs. The content does not change. And
if workflows for one host diverge over time (for example, UI workflows that
drive UI tracks alongside the shared SQL workflows), the same `requires`
mechanism keeps the shared core together and routes host-specific workflows
only to the hosts that support them.

### Links

Workflows often point to follow-up workflows that need more capabilities, such
as a diff workflow for comparing two traces. Putting those links in frontmatter
lets the generator drop them when the target file is excluded:

```yaml
---
requires: [query]
related:
  - path: heap_dump_diff.md
    when: Two dumps (before/after)
---
```

The generator renders a "Related workflows" section at the bottom of the file
from the entries that survive for the host. In the router
(`SKILL-template.md`), any line that links to a dropped file is removed, along
with any numbered section that has no surviving lines.

In all other files, a link in the body to a file the host dropped fails the
build instead of being stripped. Body links sit inside prose that assumes the
target workflow exists (for example, "if you have a second trace, follow the
diff workflow to compare them"); stripping the link alone leaves dangling
instructions behind. Failing the build forces the link to move to `related` or
the surrounding section to be split out.

### Generator

`tools/gen_skill_bundle.py` builds the bundle for one host:

```sh
# In-tree host (reads environment-references/ui.md):
tools/gen_skill_bundle.py --host ui --layout index --out out/ui-skills \
  --drop-report ai/skills/drop_reports/ui.txt

# Out-of-tree adapter:
tools/gen_skill_bundle.py --adapter /path/to/google3.md --layout tree \
  --out out/g3-skills
```

`--host <name>` is shorthand for the in-tree adapter
`environment-references/<name>.md`, while `--adapter <path>` takes an adapter
file directly for out-of-tree hosts.

`--layout` picks the output format:

* `tree`: the full skill directory tree with `SKILL.md`, for hosts with a
  filesystem (such as `cli`).
* `index`: only the reachable `.md` and `.sql` files plus an `index.json`
  manifest, for hosts like `ui` that have no filesystem and fetch files one at
  a time through tools.

`--drop-report <path>` writes each dropped file and the action the host was
missing. Drop reports for in-tree hosts are checked into
`ai/skills/drop_reports/`, so changing which files a host receives shows up in
review.

The generator is the only supported way to package the skill. Each host
generates its bundle from the same commit as the `trace_processor` it runs:
the UI at UI build time, google3 at import, coding agents at release.
Android CLI pins a released bundle.

### Applied to `android_memory/`

| File | `requires` | Coding agent (`cli`) | UI (`ui`) |
| --- | --- | --- | --- |
| `heap_dump.md` | `query` | yes | yes |
| `heap_dump_caching_optimizer.md` | `query` | yes | yes |
| `native_heap.md` | `query` | yes | yes |
| `java_allocation_profile.md` | `query` | yes | yes |
| `system_trace_memory.md` | `query` | yes | yes |
| `system_trace_memory_compare.md` (split out) | `query`, `multi_trace` | yes | no |
| `recording_memory_signals.md` (split out) | `record` | yes | no |
| `heap_dump_diff.md` | `query`, `multi_trace`, `python` | yes | no |
| `heap_profile_diff.md` | `query`, `multi_trace`, `python` | yes | no |
| `heap_dump_cluster.md` | `query`, `python` | yes | no |

## Alternatives considered

### Comment markers

Wrap host-specific sections inside markdown files with HTML comments (for
example, `<!-- host:cli:begin -->` ... `<!-- host:cli:end -->`) and strip
marked regions per host.

Con:

* The structure of the skill is hidden in comments inside the prose, and
  removing a region from the middle of a paragraph leaves dangling references
  or broken grammar.
* Markers name hosts, so adding a host means visiting every marker.
* Hosts that read the raw files see the markers.

### Every action as an MCP tool

Pro:

* One typed interface across hosts.

Con:

* Coding agents get no benefit: the CLI is already well-shaped and our evals
  showed setup steps cost a lot for them. Actions can map to tool calls in the
  UI adapter and to shell commands in the CLI adapter.

### Embed the content in `trace_processor`

The binary carries the guide, so the content always matches the tool.

Con:

* The UI (Wasm) and Android CLI have no native binary to ask.
* It only changes how the content is delivered, not how it is split. It is
  compatible with this design as a way to deliver the coding agent bundle.

### Per-host forks

Con:

* This is what Android CLI does today, and the copies drift.

## Open questions

* Is the action list enough for google3 and Android CLI?
* Should the Python workflows be rewritten in SQL so more hosts can run them?
* How do we run evals per host? A host can include a workflow and still do
  badly on it if its adapter's version of an action is weak.
