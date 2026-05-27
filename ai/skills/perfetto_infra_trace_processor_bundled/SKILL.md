---
name: perfetto-infra-trace-processor-bundled
description: Load whenever ANY other Perfetto skill is loaded in a
  plugin install. Defines the exact invocation path for the
  `trace_processor` binary bundled inside this plugin. Other Perfetto
  skills (querying-traces, workflow-*) write bare `trace_processor`
  in their shell examples; this skill tells you what to substitute it
  with so commands actually find the binary. The fallback equivalent
  (`getting-trace-processor`) is the OSS curl-install path used only
  when no plugin is installed.
targets: [claude-code, codex]
---

# Bundled `trace_processor` — exact invocation

This plugin ships `trace_processor` inside its own `bin/` directory.
Do **not** download it again, do **not** modify `PATH`, and do **not**
assume `trace_processor` is on `PATH` — it isn't. Instead, always
invoke the binary through the absolute path the host agent exposes
via its plugin-root environment variable.

## The substitution rule

Wherever another Perfetto skill writes `trace_processor ...`,
substitute the absolute path for your agent:

| Agent           | Substitute `trace_processor` with                       |
| --------------- | ------------------------------------------------------- |
| Claude Code     | `"$CLAUDE_PLUGIN_ROOT/bin/trace_processor"`             |
| Codex           | `"$PLUGIN_ROOT/bin/trace_processor"`                    |

So for example, the querying skill's

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
The `perfetto-infra-querying-traces` skill documents the
`TraceProcessor(addr=...)` API.
