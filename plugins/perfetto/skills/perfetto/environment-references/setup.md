# Getting `trace_processor` working

Two things must be set up once per session: `$SKILL_ROOT` and
`trace_processor`.

## 1. Set `$SKILL_ROOT`

Every file this skill references — workflow markdown, reference docs,
helper scripts, and the `trace_processor` wrapper — is named by a path
of the form `$SKILL_ROOT/...`, relative to the **skill root** (the
directory holding this skill's `SKILL.md`). Set it once, to the absolute
path of the directory you loaded `SKILL.md` from:

```sh
# Substitute the directory this SKILL.md lives in.
export SKILL_ROOT="/absolute/path/to/skills/perfetto"
```

## 2. Put the bundled `trace_processor` on the `PATH`

The skill ships a `trace_processor` wrapper at
`$SKILL_ROOT/bin/trace_processor`. Make it invokable for this session:

```sh
chmod +x "$SKILL_ROOT/bin/trace_processor"  # some installs lose the exec bit
export PATH="$SKILL_ROOT/bin:$PATH"
trace_processor --version                   # smoke test
```

After this, every bare `trace_processor ...` command in this skill works
verbatim. On Windows, skip the `PATH` setup and invoke it as
`python "$SKILL_ROOT/bin/trace_processor" ...` instead.

Notes:

- The first invocation downloads the prebuilt native binary (picking the
  right one for the host platform) into
  `~/.local/share/perfetto/prebuilts/` and caches it; only the first
  call pays the download cost.
- Do **not** download `trace_processor` separately — the wrapper is
  pinned to the skill's release.
- If the user's environment has its own mandatory `trace_processor`
  (Google-internal, OEM build environments, CI images), prefer that
  team-specific setup instead.
