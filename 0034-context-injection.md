# Perfetto UI: Context injection into the assistant

**Authors:** @stevegolton

**Status:** Draft

## Introduction

This doc specifies **context injection** - how the assistant feeds the model
what the user is currently looking at, the mechanism behind **click-to-context**.
It covers the per-turn `<ui_context>` payload, the context-provider registration
API, the context strip UI, ideas for making arbitrary (Mithril) surfaces
context-aware, and the text-first / images policy.

The mechanism here is **ambient context**: it is derived automatically from what
the user is currently looking at (selection, page, viewport) and pulled fresh
each turn, with no deliberate action required. This is distinct from
**user-controlled (pinned) context** - things the user explicitly parks in
context so they persist - which is sketched as a future extension at the end of
this doc. Unless stated otherwise, "context" below means ambient context.

This is one of the docs split out from
[RFC-0029](0029-intelletto.md), the top-level coordination doc for the AI
assistant work. The assistant UX, agent loop, and system-prompt assembly live in
[RFC-0032](0032-embedded-assistant.md); the provider-agnostic plumbing in
[RFC-0033](0033-llm-framework.md); the tool and skill surface in
[RFC-0035](0035-tools-and-skills.md). The wider motivation is in
[RFC-0025](https://github.com/google/perfetto/discussions/5763).

## Context Injection

This section describes the mechanism behind **click-to-context** (see UX in
[RFC-0032](0032-embedded-assistant.md)). For a seamless integration with the UI - the
model will need to know what the user is currently looking at to provide more
context around a prompt - similar to how selecting lines of code in an IDE can
provide crucial context that would be laborious to try and describe in prose.

In an ideal world, any UI component could be clicked and brought into
context. We're not there yet - making every UI element ingestible is a
massive job - so the initial phase exposes the state that already exists,
most of it already available on the global trace object (and the rest easily
injected from the relevant plugin that owns that piece of UI).

For example:

- Current page
- Timeline selection
- Currently selected node in the nodegraph
- Selected SQL code in the query page
- Pinned tracks

To deliver this to the model, contextual information is serialized and appended
to each and every user turn prompt. Each bit of context is intentionally small
and thus can be appended to every single prompt without worrying about blowing
out the context window. If we do need to expose larger pieces of information to
the model we should use a tool.

The alternative - sending context only when it changes - adds ambiguity: the
model has to search back through the history to find the latest context, which
may degrade as it moves through the context window. Just keep it small and add
it to every prompt.

The context information is wrapped in tags to distinguish it from the user
message like so:

```txt
<ui_context>page: timeline, selection: slice 42…</ui_context>
```

Note: There's nothing special about these tags, but they will be explained in
the system prompt added by the assistant plugin. Something along the lines of:

```txt
If you see tags like `<ui_context>`, this is the harness providing extra
information about what the user is currently looking at.
```

A summary of the current context is rendered as a set of chips in the chatbot's
sidebar above the prompt input box. The chat window has a **context
strip** directly above the input box that lists, in plain language, exactly what
context the next prompt will carry (page, selection, viewport).

Behaviour:

- Chips update **live** as the user clicks around - make a new timeline
  selection and the strip changes immediately, so it's obvious what the model
  will see on the next send.
- Each item has a **toggle** to exclude it from the next prompt (ask a
  general question without the model fixating on the current selection).
- Expanding an item shows the **raw payload** that would be sent - no hidden
  context.

Plugins can supply additional custom context by registering context providers
with the core registry. A single `getContext()` callback returns both the
plain-language summary shown on the chip and the raw payload sent to the model -
one source of truth, so what the user sees and what the model receives cannot
drift apart (the trust & reliability property in
[RFC-0032](0032-embedded-assistant.md)). Returning `undefined` means "nothing
relevant right now": the chip disappears and nothing is sent. The harness owns
chip rendering, the include/exclude toggle, and the expand-to-raw-payload view;
the provider only supplies content.

For example:

```ts
trace.llm.registerContextProvider({
  id: "dev.perfetto.Timeline#selection",

  // Optional: invariant explanation of the payload format (units, what ids
  // mean, which tools accept them). Injected once into the system prompt -
  // NOT repeated with every user message.
  description: `Timeline context payloads (type: "track_event"):
- "ts" and "dur" are in nanoseconds; "ts" is relative to trace.start_ts.
- "eventId" joins against the "id" column of the "slice" table.
- "trackUri" is accepted verbatim by the select_track tool.`,

  getContext(): ContextSnapshot | undefined {
    const sel = trace.selection.selection;
    if (sel.kind !== "track_event") return undefined;
    return {
      // Plain-language summary, shown on the chip in the context strip.
      summary: `Selected slice: ${sel.name} (dur=${formatDuration(sel.dur)})`,

      // JSON-serialisable payload sent to the model inside <ui_context>,
      // and what the user sees when they expand the chip.
      data: {
        type: "track_event",
        trackUri: sel.trackUri,
        eventId: sel.eventId,
        ts: sel.ts,
        dur: sel.dur,
        name: sel.name,
      },
    };
  },
});
```

Often the payload alone isn't self-explanatory - the model can't infer units or
what an id joins against for example. An invariant explanation can be supplied
via the provider's optional `description`, which the harness folds into the
**system prompt** (assembled by the assistant - see
[RFC-0032](0032-embedded-assistant.md)) rather than repeating it alongside every
payload:

- **Sent once, not per turn** - it lands in the cache-stable prefix, while the
  per-turn `<ui_context>` block stays data-only. Anything that changes per turn
  belongs in the payload, not the description.
- **Included only while the provider is registered** - no timeline plugin
  loaded, no timeline payload explanation burning prefix tokens. (Keyed on
  registration, not on whether `getContext()` currently returns data - the
  latter changes per click and would bust the cache.)
- **Colocated with the payload it describes** - registered together, written
  next to the code that builds `data`, so the explanation is less likely to be
  orphaned or drift out of sync.

## Making UI surfaces context-aware (Mithril)

> **Status: implementation ideas, not a committed design.**

The mechanism above only works if a surface can hand the harness a snapshot of
itself. Today that's easy for a handful of well-known things - selection, page,
viewport - because they already live on the global trace object. The "in an
ideal world, any UI component could be brought into context" goal needs a cheap,
uniform way for an _arbitrary_ component to opt in, without every plugin
reinventing it and without anyone scraping the DOM. A few ideas, all leaning on
the fact that the UI is Mithril:

**Tie provider lifetime to component lifetime.** A context provider's natural
scope is "while this thing is on screen", and Mithril already hands us the
hooks. A small wrapper component (or a helper called from `oncreate` /
`onremove`) registers a scoped context provider when the surface mounts and
tears it down when it unmounts - so a details panel that's currently open
contributes context, and closing it makes the chip disappear with no manual
bookkeeping. It's the same "included only while registered" property the
descriptions already rely on, but driven by the vdom lifecycle instead of a
hand-written register/unregister pair.

```ts
// A component opts in by wrapping the content it wants to be askable about.
m(ContextSurface, {
  id: "dev.perfetto.MyPanel#current",
  getContext: () => ({
    summary: `Viewing ${thing.name}`,
    data: thing.serialize(),
  }),
}, /* ...children */);
```

**Derive context from state, never the DOM.** The snapshot should read the same
model the component renders from, not its rendered output - that keeps it
lossless and decoupled from layout, zoom and theme (the same argument as
text-first, below). In practice the `getContext` closure reads the very fields
`view()` reads.

**Reuse the redraw cycle for "live" chips.** The context strip updates live as
the user clicks around. Rather than a bespoke subscription, the harness can
re-pull the registered providers on Mithril's redraw (debounced), so the strip
stays in sync using the machinery the UI already runs on. Where a surface's
state lives in an observable store, deriving both the `view()` and the chip from
that one source is what stops them drifting apart.

**Drag-to-pin.** Once a surface can describe itself, making it _draggable_ into
the context strip is a thin layer on top: the drag payload carries the provider
id (plus an instance key for surfaces that exist more than once - several Data
Explorer nodes, multiple flamegraphs), the strip becomes a drop target, and
dropping calls `pinContext()` with a frozen snapshot (see pinned context below).
Dragging is just an explicit gesture over the same self-describe capability that
powers click-to-context.

**Address instances stably.** Provider ids follow the existing
`plugin.Thing#sub` convention; for repeated surfaces, append an instance key so
a dragged or pinned reference can be re-resolved and two open panels of the same
kind don't collide.

**Make it the default, not just an opt-in.** The wrapper handles the long tail,
but most things a user points at are instances of a handful of core
components - details panels, tracks, Data Explorer nodes, flamegraphs. If those
shared base components carry the `ContextSurface` wrapper _once_, the bulk of the
UI becomes contextable for free, and only bespoke surfaces need a manual opt-in.
The north star is "anything you can see, you can ask about"; baking context into
the common containers is how we get most of the way there without a
per-component slog.

The point is that this stays incremental: a component becomes context-aware by
adding the wrapper and a `getContext` that reads its own state - no global
change, no dependency on the assistant plugin, much like registering a command.
The long tail of "make everything ingestible" then becomes a per-component
opt-in that plugin authors can knock off as they touch each surface.

## Text-first: when images are appropriate

Context - and tool results generally - are text-first, and the aim over time
is to make more of the UI expose its data in a form a text model can ingest.

In particular, **screenshotting Perfetto's own rendering of queryable data**
(the timeline, a DE table, a flamegraph) and feeding it to the model is
explicitly rejected. It is tempting because it needs no plumbing and demos well
on any view, but text is better in every way that matters:

- It's a lossy downsample of data we already hold losslessly: a timeline pixel
  column can represent thousands of culled, overlapping slices, while
  `run_query` returns the same underlying data with full precision at a
  fraction of the token cost.
- Models misread charts confidently - plausible-but-wrong numbers read off
  pixels are close to disqualifying in a measurement tool, whereas a SQL
  result is either right or visibly errors.
- It's fragile (coupled to theme, zoom, viewport, DPI) and creates no reusable
  capability, unlike making components expose their data textually, which
  serves chips, tools, and external agents alike.

Image input _is_ in scope where the pixels carry information the trace tables
don't:

- **Images embedded in the trace itself** - some traces capture screenshots /
  framebuffers / layer snapshots (e.g. Android's screenshots track, surface
  captures). These are first-class trace artifacts recording what was actually
  on screen at time T - "the frame was delivered but the screenshot shows it was
  blank" is unrecoverable by SQL. Exposing these fits the existing design with
  no new concepts: a `get_screenshot(ts)`-style tool or simply a query, and/or a
  context provider triggered by selecting a screenshot slice. Implementation
  is out of scope here beyond noting the one plumbing prerequisite: the Provider
  layer's neutral request format must be able to carry image parts in messages
  and tool results (a [RFC-0033](0033-llm-framework.md) concern; all major
  backend APIs support this).
- **Images pasted from outside Perfetto** - a monitoring dashboard, a bug
  report screenshot, an architecture diagram: "here's the latency spike our
  monitoring caught at 14:32, find what caused it in this trace". The image
  carries context that lives nowhere in the trace tables, so there is no SQL
  alternative. Also punted, same prerequisite. This is inherently
  user-driven - the entry point for it is pinned context (below).

In short: images are accepted when they carry evidence the trace tables don't
contain, not as a substitute for querying data we already have.

## User-controlled (pinned) context

> **Status: future extension, sketch only.** Everything above is ambient context
> - derived automatically and pulled fresh each turn. This section sketches the
> complementary half: letting the user *deliberately* park something in context
> so it persists. Not Phase 1; recorded here so the ambient design above doesn't
> have to change to accommodate it later.

Ambient context tracks whatever the user happens to be looking at and is
recomputed every turn, so it is ephemeral: navigate away and it's gone.
Sometimes the user wants the opposite - to say "keep *this* in mind" and have it
ride along regardless of where the selection drifts next. The two are
complementary, and the context strip already hints at the symmetry: the per-chip
toggle lets the user *remove* ambient context; deliberate injection is the *add*
side of the same strip.

The design goal is to add this as a second **lifetime** over the existing data
shape, not a new concept:

- **Same `ContextSnapshot`** (`summary` + `data`), same expand-to-raw-payload
  view, same trust property.
- **Pinned items are stored by the harness** (conversation-scoped) and injected
  every turn until the user removes them. The chip affordance is *remove* (✕)
  rather than *toggle*, and it's styled as sticky.
- Pinned items could carry a distinct tag (e.g. `<pinned_context>` vs
  `<ui_context>`) so the model can tell "the user deliberately gave me this"
  from ambient state.

Three plausible entry points, roughly in priority order:

1. **Promote an ambient chip.** A 📌 on a live chip *freezes the current
   snapshot* into a pinned item - select a slice, pin it, and "slice 42" rides
   along even after navigating away. Cheapest and highest-value: it reuses the
   context providers already written, with no new registration.
2. **"Add to assistant context" actions** on UI elements (context menu, details
   panel), each handing the harness a `ContextSnapshot`. The explicit
   counterpart to click-to-context, and the home for things with no ambient
   provider.
3. **Free-form paste** of text or an image into the context strip - also the
   entry point for the "images pasted from outside Perfetto" case above.

Two things to get right:

- **Freeze = snapshot, so staleness is real.** A pinned item is point-in-time.
  The conversation is already trace-scoped (so `eventId` / `trackUri` stay
  valid), but the displayed state may diverge from live - pinned chips should be
  marked as snapshots, not pretend to re-derive.
- **Size.** Ambient items are deliberately tiny so they can ride every turn.
  User-pinned content (a pasted log, a query result) can be large, so pinned
  injection should lean on the same escape hatch tools use - a hard cap, or a
  handle + summary for large payloads - rather than inlining everything.

API-wise this is a small addition next to `registerContextProvider`: an
imperative `trace.llm.pinContext(snapshot)` backed by a harness-owned,
conversation-scoped store, plus the promote / remove UI. The provider contract
above is unchanged.
