# Perfetto UI: Intelletto - AI Assistant (coordination)

**Authors:** @stevegolton

**Status:** Draft

## Introduction

Reading a trace well takes deep, domain-specific knowledge of the system being
traces - plus a fair amount of Perfetto UI fluency to get at it - a barrier that
keeps most of Perfetto's power in the hands of a few experts. This doc proposes
an AI assistant, and the LLM framework behind it, that lets anyone investigate a
trace and drive the UI in natural language: ask why a frame was janky, turn a
question into SQL, build a Data Explorer graph, or jump to the right point in
the timeline.

It is the **coordination doc** for the work involved with integrating this new
tooling into the Perfetto UI: it owns the overall motivation, the component
breakdown, the cross-cutting risks, and the roadmap, and links out to a
dedicated RFC for each component. It does **not** specify any component itself -
each lives in its own doc (see "What we're actually building" below). For
concrete examples of what the assistant can do, and the case for embedding it in
the UI, see the embedded-assistant doc
([RFC-0032](0032-embedded-assistant.md)).

### Framing

[RFC-0025](https://github.com/google/perfetto/discussions/5763) establishes the
motivation and demand for AI tooling in Perfetto - who it serves, the problems
it addresses, and the evidence behind it. This doc and its children deliberately
take that case as given and cover the **implementation** in the UI. Review
comments about _whether_ to build this belong on that doc; these are about _how_.

There is also prior art within the codebase: `com.google.PerfettoMcp` already
offers similar but simpler chatbot-style functionality, and will likely be
replaced by the tool described in these docs (see the migration subsection
under Roadmap, and [RFC-0032](0032-embedded-assistant.md)).

### Risks

Adding potentially controversial features to a well-established tool carries
risk:

- **Privacy reputation.** AI and LLMs have a poor reputation on privacy - long
  one of Perfetto's strengths - so any integration risks undermining the trust
  we've built there. To put minds at ease up front: Perfetto OSS will always
  remain model-agnostic, with no model or provider baked in - the user is always
  free to bring their own and decide where their data goes (see the last point
  below).
- **Useless features degrade the experience.** AI bolted onto established
  software without clear value has repeatedly drawn criticism and, in many cases,
  degraded the very experience it set out to improve.
- **Hallucinations and the cost of being wrong.** LLMs can state falsehoods
  confidently, and a plausible-but-wrong answer can erode confidence in the tool
  as a whole - cutting against Perfetto's long-standing principle of only
  showing what we know to be true. While hallucinations can't be eliminated
  entirely, the goal is to make the value the assistant adds clearly outweigh
  the harm done by the occasional falsehood.
- **Hosted users can't opt out by forking.** Perfetto is open source so folks
  can in theory strip out any feature they don't want, but a lot of users use
  the hosted build at ui.perfetto.dev. Thus, every feature has to earn its place
  on the value it genuinely adds, rather than simply being an attempt to
  shoe-horn in the latest tech trends.
- **Disablable, possibly off by default.** There should be a way to turn off all
  AI related tooling entirely. We still want to advertise these features - but
  only to make users who'd benefit aware of them and drive adoption, not to push
  AI on anyone.
- **The value is the tools and skills, not the model.** The real value of this
  feature isn't in the weights of the model used: it's the tools and the library
  of domain-specific skills the model draws on. The model itself is largely
  interchangeable, and Perfetto stays model-agnostic - users are always free to
  bring whatever model they prefer.

> **Note on the name.** _Intelletto_ is a **codename** for the assistant, used
> in the plugin id (`dev.perfetto.Intelletto`); these docs otherwise just say
> "the assistant". The name may be subject to change.

### Scope

These docs cover the implementation in the OSS codebase - the provider-agnostic
plumbing. They make no judgement about which backends are used or what data is
acceptable to send to them: trace contents are sent to whichever endpoint the
user configures, and any data-egress / privacy policy is a deployment concern
layered on top, out of scope here. Likewise, API key handling is up to the
user or specific deployment.

These docs also don't cover classic ML models for uses such as classifying
traces. While the assistant could certainly make use of ML-powered tools in
the future, this work is focused on the assistant, which will leverage LLMs.

## What we're actually building

The proposal breaks down into four components, each with a different value
proposition and a different degree of novelty. This doc specifies none of them
directly; each has its own RFC, linked below. In brief:

1. **The embedded assistant** — the sidebar chat itself, and what embedding buys:
   click-to-context and the ability to act on the same view the user is looking
   at. **[RFC-0032](0032-embedded-assistant.md)** (UX, agent loop, system prompt,
   conversation management).
2. **The LLM framework** — the provider-agnostic Provider → Config → Model
   gateway in the UI core: a shared backend any plugin or core feature can
   request a model from, with providers (and keys) pushable by extension servers
   so managed deployments are plug-n-play. **[RFC-0033](0033-llm-framework.md)**.
3. **The extensible surface** — context, tools and skills registered by plugins
   and the core, so the assistant's capability grows with the codebase instead of
   a hand-maintained list. **[RFC-0034](0034-context-injection.md)** (context
   injection) and **[RFC-0035](0035-tools-and-skills.md)** (tools & skills).
4. **External harness integration** — the same tool surface exposed to Gemini
   CLI / Claude Code / Cursor-style agents over a bridge via trace processor.
   **[RFC-0036](0036-external-agent-mcp.md)**.

## Roadmap

**Status: a working prototype exists.** The screenshots in the child docs are
taken from it; it covers most of Phase 1 (the Provider/Config/Model stack with
Gemini and OpenAI-compatible providers, the sidebar with context chips and
tool use, the core tool surface). Phase 1 is therefore largely a landing
plan - review, hardening, and upstreaming existing code - rather than
speculative work, and the designs in the child docs are informed by it rather
than hypothetical.

See: https://github.com/google/perfetto/pull/6209

- **Phase 1 — core plumbing** (mostly prototyped): Provider/Config/Model
  config layers, Gemini provider, agent sidebar, basic context injection
  (page + selection + viewport), core tool surface (SQL queries, selection,
  timeline navigation, Data Explorer state). Embedded assistant only - no
  bridge or external-harness support of any kind. Done when: a user with a
  configured provider can run the intro's example prompts end-to-end in the
  sidebar.
- **Phase 2 — richer context & extensibility**: click-to-context on anything,
  additional providers, tools from other plugins, skills integration,
  external agent conduit via TP, merge with and deprecate PerfettoMcp. Done
  when: a third-party plugin can register a tool, a skill, and a context
  provider without core changes, and an external harness can drive the UI
  through the TP conduit.
- **Phase 3 — advanced**: context compaction, more provider types, extension
  server integration, first-party extensions for external harnesses (if
  warranted), richer tool surface as plugins add their own.

### Migration from `com.google.PerfettoMcp`

`PerfettoMcp` is folded into this design and deprecated once the assistant's
tool surface covers it (Phase 2, alongside the external-agent conduit). The
details live in [RFC-0036](0036-external-agent-mcp.md).

## Open questions

The open questions now live in the docs that own each area:

1. ~~**Build vs. library for the provider layer**~~ - decided (build); see
   [RFC-0033](0033-llm-framework.md#open-questions).
2. **Tools vs. commands** - whether tools are their own registration mechanism
   or commands grow optional schemas + an allowlist; see
   [RFC-0035](0035-tools-and-skills.md#open-questions).
3. **External-agent conduit** - the whole of
   [RFC-0036](0036-external-agent-mcp.md) is the least settled part of the
   proposal.
