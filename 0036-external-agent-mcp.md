# Perfetto UI: External agents via `trace_processor` as a conduit

**Authors:** @stevegolton

**Status:** Draft

## Introduction

This doc covers exposing the assistant's tool/skill/context surface to
**external coding agents** (Gemini CLI, Claude Code, Codex, Cursor, …) by
turning `trace_processor` into an MCP conduit to the running UI.

This is the most speculative of the docs split out from
[RFC-0029](0029-intelletto.md), the top-level coordination doc - none of it is
Phase 1, and it's the least settled part of the whole proposal. It's written
down mainly so the tool, skill and context contracts in the sibling docs don't
have to change to accommodate it later; treat the specifics below as a candidate
direction, not a committed design. The in-panel assistant (UX, agent loop,
system prompt) lives in [RFC-0032](0032-embedded-assistant.md); the plumbing in
[RFC-0033](0033-llm-framework.md); context injection in
[RFC-0034](0034-context-injection.md); the tools and skills this surface
re-exposes in [RFC-0035](0035-tools-and-skills.md). The wider motivation is in
[RFC-0025](https://github.com/google/perfetto/discussions/5763).

## External agents via `trace_processor` as a conduit

Most of the assistant work is about the in-panel assistant, but the same tool
surface (see [RFC-0035](0035-tools-and-skills.md)) should also be reachable by
**external coding agents** (Gemini CLI, Claude Code, Codex, Cursor, etc.). MCP
servers for data sources are common; what's less common - and what's novel here -
is an agent driving a _running UI_ the user is watching. The value is twofold:
meeting users in the harnesses they already pay for and trust, and - longer
term - a scriptable DevTools-for-Perfetto automation surface that falls out of
the same commands-with-schemas work.

None of this is Phase 1 work: the bridge and all external-harness support is
punted to Phase 2 at the earliest (see Roadmap in
[RFC-0029](0029-intelletto.md)); it's designed here so the tool and skill
contracts don't have to change to accommodate it later.

The cleanest path for the common Perfetto deployment is to **reuse the existing
`trace_processor --httpd` connection** as the conduit: TP advertises an MCP
server and bridges tool calls to the UI over the existing websocket connection,
so calls round-trip out to the browser and back. No
new transport, and it works in any browser (not gated on browser-extension
features).

Parity with the in-panel assistant is not free, though: an external agent owns
its own harness, so the channels we control in-panel degrade per channel:

- **Tool descriptions transfer for free** - they travel with the MCP tool
  definitions, and the when-to-call guidance already lives there rather than
  in the system prompt.
- **System prompt becomes MCP `instructions`**: we can't touch the external
  agent's system prompt, but the MCP initialize handshake carries a server
  `instructions` field that well-behaved clients fold into context. A
  condensed application brief plus the payload-format conventions go there.
  Advisory and size-constrained, but workable.
- **Click-to-context inverts from push to pull**: generically, there is no
  per-turn injection hook into someone else's harness, so the per-turn
  `<ui_context>` block (see [RFC-0034](0034-context-injection.md)) becomes a
  `get_ui_context()` tool returning exactly what the context providers would
  have emitted. The model has to _decide_ to call it - steered via the server
  instructions ("call `get_ui_context` before answering questions about
  'this'") - which is strictly worse: the user clicks a slice and the external
  agent doesn't know unless it asks. Click-to-context largely does not survive
  the bare conduit - this gap is the main reason the embedded assistant exists.
- **`read_docs` / self-help transfers well** - it's already pull-based, so
  external agents bootstrap the same way the in-panel model does.

Much of what the bare conduit loses could be clawed back by adding UI related
tooling to our harness extension that we already build. Our Claude Code and
Codex plugins for example, could be extended to push UI context per-turn (a
prompt-submit hook that calls `get_ui_context()` and prepends the result),
install our UI skills in native format, and carry the application brief -
substantially narrowing the gap to the embedded assistant. But each extension is
a bespoke integration to build, test, and maintain against someone else's moving
harness, multiplied per harness, extra work on top of a conduit that is itself a
Phase 2 item, so extensions land later still, if at all. The conduit's generic
posture (tools, MCP `instructions`, pull-based context) is the supported
baseline.

WebMCP / `navigator.modelContext` is the browser-native standard this is
heading toward, and we'd register tools there too since that's the direction
of travel - but today it's an experimental draft API behind browser
flags/origin trials, and reaching it from an external agent still needs a
browser extension + local MCP shim, so it doesn't really help us yet. The
TP-conduit path is the pragmatic one for now; treat WebMCP as something to
track, not depend on.

## Migration from `com.google.PerfettoMcp`

`PerfettoMcp` already does a version of this - exposing the trace to an LLM -
but in a more limited way. Rather than running two overlapping mechanisms,
the plan is to fold its useful pieces into this design and **deprecate
PerfettoMcp** once the assistant's tool surface covers what it did (a Phase 2
step, alongside the external agent conduit it overlaps with).
