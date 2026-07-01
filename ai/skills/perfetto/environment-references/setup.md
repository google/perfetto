# Environment setup: `$SKILL_ROOT` and `trace_processor`

Every install of this skill, whatever the agent, ships the same layout:
the skill directory itself, with a `trace_processor` wrapper inside it
at `bin/trace_processor`. This file defines the two anchors everything
else relies on: `$SKILL_ROOT` and the `trace_processor` invocation.

## Skill files live under `$SKILL_ROOT`

Every file this skill references — workflow markdown, the reference
docs, the SQL/Python helper scripts under a `scripts/` dir, and the
`trace_processor` wrapper — is named by a path of the form
`$SKILL_ROOT/...`, relative to the **skill root** (the directory
holding this skill's `SKILL.md`). The skill is *not* installed in your
working directory, so these paths only resolve once `$SKILL_ROOT`
points at the install location.

`$SKILL_ROOT` is not exported by the host. Set it yourself, once, at
the start of your session, to the absolute path of the directory you
loaded this skill's `SKILL.md` from (your agent reported it when the
skill loaded):

```sh
# Substitute the directory this SKILL.md lives in.
export SKILL_ROOT="/absolute/path/to/skills/perfetto"
```

After that, every `$SKILL_ROOT/...` path in this skill works verbatim,
both as a shell argument (e.g. `--query-file
$SKILL_ROOT/workflows/android_memory/scripts/triage_dominator_path.sql`)
and when you need to open a referenced file. Your working directory is
irrelevant — the paths are absolute once `$SKILL_ROOT` is set, so you
never need to `cd` into the skill.

## The `trace_processor` substitution rule

Wherever another reference or workflow writes `trace_processor ...`,
substitute the bundled wrapper:

```sh
python3 "$SKILL_ROOT/bin/trace_processor" ...
```

So for example, the querying reference's

```sh
trace_processor query TRACE_FILE "SELECT ts, dur FROM slice LIMIT 10"
```

becomes:

```sh
python3 "$SKILL_ROOT/bin/trace_processor" query TRACE_FILE "SELECT ts, dur FROM slice LIMIT 10"
```

Notes:

- Invoke it through `python3` (`python` on Windows): depending on how
  the skill was installed, the file may not carry an executable bit.
- Do **not** download `trace_processor` separately, do **not** modify
  `PATH`, and do **not** assume `trace_processor` is on `PATH` — it
  isn't.
- **Environment overrides win.** Google-internal users, OEM build
  environments, and CI images often have their own mandatory
  `trace_processor` path. If the user is in such an environment, prefer
  their team-specific setup over the bundled wrapper.
- **Fallback.** If `$SKILL_ROOT/bin/trace_processor` is missing (an old
  or partial install), fetch the official wrapper instead — it is the
  same script:

  ```sh
  curl -LO https://get.perfetto.dev/trace_processor
  chmod +x trace_processor
  ./trace_processor --version    # smoke test
  ```

## First-run note

The first invocation of `bin/trace_processor` downloads the prebuilt
native shell binary (picking the right one for the host platform) into
`~/.local/share/perfetto/prebuilts/` and caches it. Subsequent calls
reuse the cached binary, so only the first call pays the download cost.

## Python client (for long-running RPC mode)

`$SKILL_ROOT/infra-references/querying.md` recommends starting
`trace_processor server http TRACE_FILE` once and connecting from
Python so iteration doesn't re-parse the trace on every query. That
requires the `perfetto` Python package (and `protobuf`), which the
wrapper does not bundle.

### Default: an isolated venv

Modern macOS / Debian / Ubuntu Python installs (PEP 668) refuse global
`pip install` by default, and mixing the Perfetto client into the
system interpreter is a recipe for version conflicts. The opinionated
path is a dedicated venv:

```sh
python3 -m venv ~/.venv/perfetto
~/.venv/perfetto/bin/pip install -U perfetto protobuf
# Optional: pandas, only if you want .as_pandas_dataframe() to work.
~/.venv/perfetto/bin/pip install -U pandas

# Sanity check.
~/.venv/perfetto/bin/python -c \
  "from perfetto.trace_processor import TraceProcessor; print('ok')"
```

Then invoke Python from the querying reference as
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
  to that. This is the OSS default, not a mandate.

## Verifying the full setup

A one-shot end-to-end check that the binary and the client agree:

```sh
# Start the server on a small trace in the background, on a random port
# to avoid clashing with any other trace_processor server / the UI.
PORT=$((9100 + RANDOM % 900))
python3 "$SKILL_ROOT/bin/trace_processor" server http --port $PORT \
    /path/to/some_trace.pftrace &
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
follow `$SKILL_ROOT/infra-references/querying.md`.
