# Getting `trace_processor` working

Two things are set up once per session: `$SKILL_ROOT` and
`trace_processor`. (`SKILL.md` repeats this; it is here so workflow
files have something to point at.)

1. **`$SKILL_ROOT`** is the absolute path of the directory containing this
   skill's `SKILL.md`. Every file the skill references is written as
   `$SKILL_ROOT/<path>`, never relative to the file doing the referencing,
   because the skill is loaded from a plugin directory, not from the
   user's workspace.

2. **`trace_processor`** is bundled at `$SKILL_ROOT/bin/trace_processor`:

```sh
export SKILL_ROOT="/absolute/path/to/skills/perfetto"
chmod +x "$SKILL_ROOT/bin/trace_processor"   # some installs lose the exec bit
export PATH="$SKILL_ROOT/bin:$PATH"
trace_processor --version                     # smoke test
```

- The first invocation downloads the prebuilt binary for the host
  platform into `~/.local/share/perfetto/prebuilts/` and caches it.
- Do not download `trace_processor` separately: the wrapper is pinned to
  the release the skill was built for.
- On Windows skip the `PATH` step and run
  `python "$SKILL_ROOT/bin/trace_processor" ...`.
- If the environment mandates its own `trace_processor` (Google internal,
  OEM build environments, CI images), prefer that team-specific setup.
