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
`../infra-references/querying.md` documents the `TraceProcessor(addr=...)`
API.
