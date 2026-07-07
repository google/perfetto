# Perfetto Agent Extensions

Per-agent **extension manifests** that let supported coding agents install
the Perfetto skill. The skill itself lives in
[`ai/skills/perfetto/`](../skills/) and is shared across agents; only the
install-time manifest is per-agent.

This directory is the source of truth. The release pipeline assembles it
(with `ai/skills/` and the `tools/trace_processor` wrapper) into the
[`ai-agents`](https://github.com/google/perfetto/tree/ai-agents) branch.
See [RFC-0026](https://github.com/google/perfetto/discussions/5892) for the
design and
[`docs/getting-started/using-ai.md`](../../docs/getting-started/using-ai.md)
for how users install.

## Layout

```
ai/extensions/
├── claude-code/
│   ├── plugin.json              → .claude-plugin/plugin.json
│   └── marketplace.json         → .claude-plugin/marketplace.json
└── codex/
    ├── plugin.json              → .codex-plugin/plugin.json
    └── marketplace.json         → .agents/plugins/marketplace.json
```

The arrows show where each file lands on the `ai-agents` branch. Claude Code
and Codex are the only agents that need a per-agent manifest. Every other
agent (OpenCode, Pi, Antigravity, …) is a fallback consumer installed via
`tools/agents-install` (served at
<https://get.perfetto.dev/agents-install>) — no manifest required.

The skill tree is emitted exactly once, at `plugins/perfetto/skills/`:
plugin installs consume the `plugins/perfetto/` subdir, and fallback
consumers (OpenCode's `index.json` URL, `tools/agents-install`) read the
same tree at its full path. Every install gets the identical assembled
skill, which carries the `trace_processor` wrapper at `bin/` inside the
skill, so `$SKILL_ROOT/bin/trace_processor` resolves everywhere. See
[`ai/skills/README.md`](../skills/README.md) for how the bundler
assembles the single skill.

## Versioning

`tools/release/roll-prebuilts` stamps the release version into the manifests
and `tools/agents-install` at roll time, in lockstep with the bundled
`trace_processor`. Before the first roll they carry the `0.0.0-dev` sentinel;
`build_ai_agents.py` does no rewriting.

## Authoring

- Keep manifests minimal — install metadata, not documentation.
- Skills are auto-discovered from `skills/`; manifests don't enumerate them.
- Run `tools/check_extension_manifests.py` before sending the change.
