---
name: perfetto-infra-getting-trace-processor
description: Load whenever ANY other Perfetto skill is loaded in a
  non-plugin install (no `trace_processor` is bundled here). Defines
  how to fetch the prebuilt `trace_processor` binary and the `perfetto`
  Python client so the shell commands in other Perfetto skills can
  actually run. This includes fallback-style consumers such as
  Antigravity. The plugin equivalent (`trace-processor-bundled`) points
  at a pre-installed binary instead; this skill is the OSS / fallback
  acquisition path. Teams in restricted environments may ship their own
  variant that overrides it.
targets: [fallback]
---

# Getting `trace_processor` and the Python client

This skill is one *answer* to the question "where does `trace_processor`
come from?". It is intentionally **orthogonal** to the
`querying-traces` skill: that one teaches what to do once you have a
working `trace_processor`; this one teaches how to obtain it.

There are many possible answers depending on the environment — Google
internal users, OEM build environments, CI images, and other teams
typically have their own preferred or mandatory path. **If the user is
in such an environment, prefer their team-specific version of this
skill.** Use this one when nothing more specific is available.

This is also the acquisition skill used by fallback-style agent installs
such as Antigravity, because those installs consume the root `skills/`
layout and do not receive the plugin-bundled `bin/trace_processor`
wrapper.

## Part 1 — The `trace_processor` binary

Fetch the official prebuilt binary. This is what 99% of users should
do — a build from source is only needed when developing Perfetto
itself.

```sh
curl -LO https://get.perfetto.dev/trace_processor
chmod +x trace_processor
./trace_processor --version    # smoke test
```

`get.perfetto.dev/trace_processor` is a small wrapper script that picks
the right prebuilt binary for the host platform (Linux x86_64 / arm64,
macOS, Windows) and caches it. Re-running the `curl -LO` (or just
re-invoking `./trace_processor`) is the recommended way to stay
current — the script is hash-idempotent, so it only re-downloads when
the upstream binary actually changes. **Don't move it onto `PATH`** as
a one-shot install: doing so encourages users to keep running a stale
binary indefinitely.

If a Perfetto source checkout *is* available, `tools/trace_processor`
inside that checkout does the same fetch under the hood and
additionally supports `--build` for a from-source build.

## Part 2 — The Python client (for long-running RPC mode)

The `querying-traces` skill recommends starting `trace_processor server
http TRACE_FILE` once and connecting from Python so iteration doesn't
re-parse the trace on every query. That requires the `perfetto` Python
package (and `protobuf`).

### Default: an isolated venv

Modern macOS / Debian / Ubuntu Python installs (PEP 668) refuse global
`pip install` by default, and even where they don't, mixing the
Perfetto client into the system interpreter is a recipe for version
conflicts. The opinionated path is a dedicated venv:

```sh
python3 -m venv ~/.venv/perfetto
~/.venv/perfetto/bin/pip install -U perfetto protobuf
# Optional: pandas, only if you want .as_pandas_dataframe() to work.
~/.venv/perfetto/bin/pip install -U pandas

# Sanity check.
~/.venv/perfetto/bin/python -c \
  "from perfetto.trace_processor import TraceProcessor; print('ok')"
```

Then invoke Python from the querying skill as
`~/.venv/perfetto/bin/python`, or `source ~/.venv/perfetto/bin/activate`
once per shell and use plain `python`.

### When to override the default

- **The user already has an environment manager** (`uv`, `pipx`,
  `conda`, `poetry`, an existing project venv): install `perfetto` and
  `protobuf` into that environment instead of creating a fresh
  `~/.venv/perfetto`.
- **The user explicitly wants a global install**: `pip install
  --break-system-packages perfetto protobuf` is the escape hatch on
  PEP-668 distros. Don't reach for it on the user's behalf — only
  when they ask.
- **The environment has its own packaging story** (Google internal,
  CI images with pre-baked deps, locked-down corporate Python): defer
  to that. This skill is the OSS default, not a mandate.

## Verifying the full setup

A one-shot end-to-end check that the binary and the client agree:

```sh
# Start the server on a small trace in the background, on a random port
# to avoid clashing with any other trace_processor server / the UI.
PORT=$((9100 + RANDOM % 900))
trace_processor server http --port $PORT /path/to/some_trace.pftrace &
SERVER_PID=$!
sleep 1

# Query via the Python client.
~/.venv/perfetto/bin/python - "$PORT" <<'PY'
import sys
from perfetto.trace_processor import TraceProcessor
tp = TraceProcessor(addr=f'127.0.0.1:{sys.argv[1]}')
print(next(iter(tp.query('SELECT count(*) AS c FROM slice'))).c)
tp.close()
PY

kill $SERVER_PID
```

If this prints a non-zero count and exits cleanly, the user is ready to
follow the `querying-traces` skill.
