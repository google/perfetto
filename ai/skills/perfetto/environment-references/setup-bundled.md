# Bundled `trace_processor` — exact invocation

This is the environment setup for a **plugin install** that ships
`trace_processor` inside the plugin's own `bin/` directory.

Do **not** download it again, do **not** modify `PATH`, and do **not**
assume `trace_processor` is on `PATH` — it isn't. Instead, always
invoke the binary through the absolute path the host agent exposes
via its plugin-root environment variable.

## The substitution rule

Wherever another reference or workflow writes `trace_processor ...`,
substitute the absolute path for your agent:

| Agent           | Substitute `trace_processor` with                       |
| --------------- | ------------------------------------------------------- |
| Claude Code     | `"$CLAUDE_PLUGIN_ROOT/bin/trace_processor"`             |
| Codex           | `"$PLUGIN_ROOT/bin/trace_processor"`                    |

So for example, the querying reference's

```sh
trace_processor query TRACE_FILE "SELECT ts, dur FROM slice LIMIT 10"
```

becomes (under Claude Code):

```sh
"$CLAUDE_PLUGIN_ROOT/bin/trace_processor" query TRACE_FILE "SELECT ts, dur FROM slice LIMIT 10"
```

The environment variable is always set by the host before any shell
command in a skill runs, so the path is always valid for the current
install.

## Skill files live under `$SKILL_ROOT`

Every other file this skill references — workflow markdown, the
reference docs, and the SQL/Python helper scripts under a `scripts/`
dir — is named by a path of the form `$SKILL_ROOT/...`, relative to the
**skill root** (the directory holding this `SKILL.md`). The skill is
*not* installed in your working directory, so these paths only resolve
once `$SKILL_ROOT` points at the install location.

`$SKILL_ROOT` is not exported by the host. Set it yourself, once, at
the start of your session, to the value for your agent:

| Agent           | Set `SKILL_ROOT` to                                     |
| --------------- | ------------------------------------------------------- |
| Claude Code     | `"$CLAUDE_PLUGIN_ROOT/skills/perfetto"`                 |
| Codex           | `"$PLUGIN_ROOT/skills/perfetto"`                        |

```sh
# Claude Code, for example:
export SKILL_ROOT="$CLAUDE_PLUGIN_ROOT/skills/perfetto"
```

After that, every `$SKILL_ROOT/...` path in this skill works verbatim,
both as a shell argument (e.g. `--query-file
$SKILL_ROOT/workflows/android_memory/scripts/triage_dominator_path.sql`)
and when you need to open a referenced file. Your working directory is
irrelevant — the paths are absolute once `$SKILL_ROOT` is set, so you
never need to `cd` into the skill.

## First-run note

The first invocation of `bin/trace_processor` downloads the prebuilt
native shell binary into `~/.local/share/perfetto/prebuilts/` and
caches it. Subsequent calls reuse the cached binary, so only the first
call pays the download cost.

## Python client (long-running RPC mode)

The bundled wrapper handles the binary but not the `perfetto` Python
client used to drive `--httpd` mode from a notebook. If the user
wants iterative querying via the Python client, install it into a
virtualenv:

```sh
python3 -m venv ~/.local/share/perfetto-venv
~/.local/share/perfetto-venv/bin/pip install --upgrade perfetto
```

Then drive the server from `~/.local/share/perfetto-venv/bin/python`.
`$SKILL_ROOT/infra-references/querying.md` documents the
`TraceProcessor(addr=...)` API.
