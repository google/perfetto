---
host: cli
provides: [query, multi_trace, python, record, write_file]
---
Set `$SKILL_ROOT` to the absolute path of the directory containing this
`SKILL.md`; every path below is written relative to it. Then put the
bundled `trace_processor` on the `PATH` and check it runs:

```sh
export SKILL_ROOT="/absolute/path/to/skills/perfetto"   # dir holding SKILL.md
chmod +x "$SKILL_ROOT/bin/trace_processor"
export PATH="$SKILL_ROOT/bin:$PATH"
trace_processor --version
```

The first call downloads the native binary for this platform and caches
it; later calls are instant. Do not download `trace_processor` separately.
On Windows invoke it as `python "$SKILL_ROOT/bin/trace_processor" ...`. If
the environment already mandates its own `trace_processor` (Google
internal, OEM or CI images), use that instead.
