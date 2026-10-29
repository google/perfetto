# Splitting Perfetto skills across hosts

**Authors:** @LalitMaganti

**Status:** Draft

## Problem

`ai/skills/perfetto/` is written for one host: a coding agent with a shell.
Other hosts now reuse it:

| Host | How it gets the content |
| --- | --- |
| Coding agents | `ai-agents` release branch, marketplaces, `agents-install` |
| Perfetto UI (#8266) | `gen_ui_skills.py` slices files on comment markers |
| google3 | The coding agent content, with its own `trace_processor` setup |
| Android CLI | Copies selected pieces in by hand |

Host-specific instructions are spread through the shared content: shell
commands, `$SKILL_ROOT`, Python scripts, loading two traces at once,
recording, writing report files. Each new host either edits every file to
mark what it can't do or makes another copy. #8266 adds 24 marked regions
to the skill files to support the UI alone.

We want one body of content, from which each host gets the subset it can run.
Adding a host should not require editing the content, and nothing should be
copied by hand.

## Decision

Pending

## Design

### Actions

Shared content is written against a small set of named actions instead of
host commands. The initial set:

* `query`: run SQL, or a shipped `.sql` file, on the loaded trace.
* `multi_trace`: hold two or more traces loaded at once.
* `python`: run a shipped Python script.
* `record`: record a new trace from a device or host.
* `write_file`: write a report to the user's working directory.

`query` is the baseline every host provides. A workflow says "run
`scripts/triage_dominator_path.sql`", not
`trace_processor query --remote SESSION --query-file $SKILL_ROOT/...`.

### Adapters

Each host provides one adapter file in `environment-references/`, which says
how to perform each action it supports and lists them in frontmatter:

```yaml
---
host: ui
provides: [query]
---
```

`setup.md` becomes the coding agent adapter. #8266's `ui.md` is the UI
adapter. google3 and Android CLI each add one. The adapter is the only file
that differs between hosts.

Adapters don't have to live in this repo. Google-internal, OEM and CI
environments often mandate their own `trace_processor` and setup, and today
`setup.md` can only tell the agent to "prefer that team-specific setup". Such
an environment passes its own adapter to the generator
(`--adapter <path>`) and gets the shared content with its setup in place of
ours, without forking any content. A custom `trace_processor` build can also
carry its adapter through the platform interface, as #5772 prototyped with
`GetExtraAiAgents()`, so that installing from that binary picks it up.

### Files declare what they need

Each content file lists the actions it needs beyond `query`:

```yaml
---
requires: [multi_trace, python]
---
```

The whole file is the unit of inclusion. A host gets a file only if its
adapter provides everything the file requires. Nothing is trimmed from inside
a file, so there are no markers.

If part of a workflow should reach a host that lacks an action, it goes in its
own file. For example, the "Recording the missing signals" section of
`system_trace_memory.md` moves to its own file with `requires: [record]`, and
the UI keeps the rest of the workflow.

Features a host can't support are dropped, not reimplemented. A host gains a
workflow by implementing the action it needs. The content does not change.

### Links

Of #8266's markers inside a file, 10 of 16 wrap a link to another workflow,
usually a diff workflow. These links move into frontmatter:

```yaml
---
related:
  - path: heap_dump_diff.md
    when: Two dumps (before/after)
---
```

The generator renders a "Related workflows" section from the entries that
survive for the host. The router (`SKILL.md`) is generated the same way from
each workflow's frontmatter. A link in the body of a file to a file the host
dropped fails the build, as it already does in #8266.

### Generator

`tools/gen_ui_skills.py` is generalised into
`tools/gen_skill_bundle.py --host <host>`. For each host it outputs:

* the included files, with `$SKILL_ROOT` paths rewritten for the host
* `index.json`
* a drop report listing each dropped file and the action it was missing

The drop report is checked in, so changing which files a host receives shows
up in review.

The generator is the only supported way to embed the skill. Each host
generates its bundle from the same commit as the `trace_processor` it runs:
the UI at UI build time, google3 at import, coding agents at release.
Android CLI pins a released bundle.

### Applied to `android_memory/`

| File | `requires` | Coding agent | UI |
| --- | --- | --- | --- |
| `heap_dump.md` | none | yes | yes |
| `heap_dump_caching_optimizer.md` | none | yes | yes |
| `native_heap.md` | none | yes | yes |
| `java_allocation_profile.md` | none | yes | yes |
| `system_trace_memory.md` | none | yes | yes |
| `system_trace_memory_compare.md` (split out) | `multi_trace` | yes | no |
| `recording_memory_signals.md` (split out) | `record` | yes | no |
| `heap_dump_diff.md` | `multi_trace`, `python` | yes | no |
| `heap_profile_diff.md` | `multi_trace`, `python` | yes | no |
| `heap_dump_cluster.md` | `python` | yes | no |

## Prototype

Rebase #8266 onto this design for `android_memory/` and the router: replace
the markers with `requires` and `related`, make the two splits above, and
compare the generated UI bundle and drop report against #8266's output. Then
run the evals for both hosts.

## Alternatives considered

### Comment markers (#8266)

Pro:

* Already implemented.

Con:

* The structure of the skill is hidden in comments inside the prose.
* Markers name hosts, so adding a host means visiting every marker.
* Coding agents see the raw markers.

### Every action as an MCP tool

Pro:

* One typed interface across hosts.

Con:

* Coding agents get no benefit: the CLI is already well-shaped and our evals
  showed setup steps cost a lot for them. Actions can map to tool calls in the
  UI adapter and to shell commands in the CLI adapter.

### Embed the content in `trace_processor`

Rex does this: the binary carries the guide, so the content always matches
the tool.

Con:

* The UI (Wasm) and Android CLI have no native binary to ask.
* It only changes how the content is delivered, not how it is split. It is
  compatible with this design as a way to deliver the coding agent bundle.

### Per-host forks

Con:

* This is what Android CLI does today, and the copies drift.

## Open questions

* Is the action list enough for google3 and Android CLI? We haven't checked
  what they take from the skill today.
* Should the Python workflows be rewritten in SQL so more hosts can run them?
* How do we run evals per host? A host can include a workflow and still do
  badly on it if its adapter's version of an action is weak.
