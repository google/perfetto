# Perfetto UI: Intelletto - AI Assistant

**Authors:** @stevegolton

**Status:** Draft

## Introduction

This doc proposes adding an LLM framework and AI assistant to the Perfetto UI,
letting users analyze their traces and operate the UI using natural language.

For example:

- **Answering questions about their trace**: click a janky frame and ask "why
  did this frame miss its deadline?", or "what was the main thread doing during
  this 200ms gap?"
- **Writing queries**: "show me the 10 longest binder transactions in this
  selection" becomes SQL without the user knowing the table layout.
- **Building node graphs in Data Explorer**: "break this down by process, then
  bucket by 10ms intervals."
- **Navigating the trace and selecting events**: "take me to the first GC pause
  after app startup."
- **Teaching users how to use the UI**: "how do I pin this track?" or "what does
  this counter track actually measure?"

The real value-add of embedding the assistant in the UI - as opposed to a 3rd
party harness connected via a bridge - is **click-to-context**: the user points
at elements in the UI and that context travels with every prompt, plug-n-play,
for every user. It also enables a richer UX to be developed around the
assistant, integrated more elegantly with the UI. It shapes much of the design below;
the full argument lives in the UX Design section.

A note on framing:
[RFC-0025](https://github.com/google/perfetto/discussions/5763) establishes the
motivation and demand for AI tooling in Perfetto - who it serves, the problems
it addresses, and the evidence behind it. This doc deliberately takes that case
as given and covers the **implementation** in the UI. Review comments about
_whether_ to build this belong on that doc; this one is about _how_.

There is also prior art within the codebase: `com.google.PerfettoMcp` already
offers similar but simpler chatbot-style functionality, and will likely be
replaced by the tool described in this doc (see the migration subsection
under Roadmap).

> **Note on the name.** _Intelletto_ is a **codename** for the assistant, used
> in the plugin id (`dev.perfetto.Intelletto`); this doc otherwise just says
> "the assistant". The name may be subject to change.

### What we're actually building

The proposal breaks down into four components, each with a different value
proposition and a different degree of novelty:

1. **The embedded assistant** - the sidebar chat itself. As a form factor
   this is the least novel part: every product has a chat panel now, and
   users arrive knowing how to use one. The value comes from what embedding
   buys: click-to-context (point at a slice and ask) and the
   ability to act on the same view the user is looking at (navigate, select,
   build queries). A chat panel bolted onto a tool with this much structured,
   queryable data behind it is worth far more than the same panel on a
   typical CRUD app. It also requires no setup: the external-harness
   route only serves engineers already living in a CLI agent (Claude Code,
   Gemini CLI, Cursor); the sidebar serves everyone who opens the UI, with
   zero setup.
2. **The LLM framework: plumbing, settings, and provider injection** - the
   provider-agnostic Protocol → Provider → Model gateway. Provider-agnostic
   LLM plumbing is well-trodden ground (it's what the AI SDKs do); the novelty
   is in the deployment story: providers - including API keys - can be pushed
   down by extension servers, so for users on a managed deployment the
   assistant is plug-n-play, with zero configuration. The value-add is that
   it's a _shared_ gateway: any plugin can request a model (auto summaries,
   classification), backends are swappable per deployment, and the active
   model is selected in one place.
3. **Context, tools and skills injected by plugins and the core codebase** - the
   assistant as an extensible surface rather than a fixed feature. This is the
   novel part: plugins register context providers and tools alongside
   their other UI extensions, and domain experts contribute reusable skills, so
   the assistant's capability grows with the codebase instead of being a
   hand-maintained list owned by one team. The domain knowledge lives next to
   the code that implements it, and a third-party plugin can teach the assistant
   about its own tracks and tables on equal footing with core.
4. **External harness integration** - the same tool surface exposed to Gemini
   CLI/Claude Code/Cursor-style agents over a bridge via trace processor.
   MCP servers for data sources are common; what's less common is an agent
   driving a _running UI_ the user is watching. The value-add is meeting users
   in harnesses they already pay for and trust, and - longer term - a scriptable
   DevTools-for-Perfetto automation surface that falls out of the same
   commands-with-schemas work.

### Scope

This doc covers the implementation in the OSS codebase - the provider-agnostic
plumbing. It makes no judgement about which backends are used or what data is
acceptable to send to them: trace contents are sent to whichever endpoint the
user configures, and any data-egress / privacy policy is a deployment concern
layered on top, out of scope here. Likewise, API key handling is up to the
user or specific deployment.

This doc also doesn't cover classic ML models for uses such as classifying
traces. While the assistant could certainly make use of ML-powered tools in
the future, this doc is focused on the assistant, which will leverage LLMs.

## UX

### UX Requirements

- **Natural Language Prompting**: The user can interact with the assistant
  using natural language.
- **Conversational (multi-turn context accumulation)**: Investigations are
  rarely solved one-shot. Users should be able to ask follow-up questions,
  leaning on the combined context from any previous questions and responses.
- **Context travels with the user**: Context should not be tied to a given
  page, panel or element. The user must be able to navigate the UI, bringing
  any existing chat context with them, asking followup questions within a
  different context.
- **Rich context injection**: The model should be delivered relevant
  information based on what the user is currently looking at. It should be
  able to infer the meaning of the word 'this' from the context provided.
- **Agency**: The assistant acts, it doesn't just answer. It acts for two
  distinct reasons:
  - **To gather what it needs** - rather than waiting for the user to bring
    the relevant information into context, the assistant finds it itself (e.g.
    running SQL to inspect the trace).
  - **To act on the user's behalf in the UI** - carrying out the user's intent
    by driving the UI (completing SQL queries, building Data Explorer graphs,
    moving the selection, navigating between pages) rather than only
    describing what the user should do. In both cases the user describes
    intent; the assistant carries it out, rather than only responding with
    text.
- **Teaching, not just doing**: A complement to Agency - sometimes the most
  useful thing the assistant can do is show the user _how_ to do something
  themselves, rather than silently doing it for them. Perfetto's UI is deep and
  much of its power is undiscovered; the assistant is well placed to surface the
  relevant feature, query, or workflow and explain it.

### UX Design

The assistant's UX surface is centered around three main concepts:

- **Chat in a sidebar**: This is a very common UX found in lots of AI tools, and
  for a good reason: it presents the chat history in one place and provides a
  persistent, always-available prompt box which can be used to ask
  followup questions given the current context. Being in a sidebar means it
  follows the user around the UI even through page flips. It should be an
  obvious UX for anyone who's used an AI powered tool before.
- **Click-to-context**: The user should be able to click on UI elements using
  the mouse and bring them into the context. This is a very natural way to
  interact with a UI, and being able to click on something then ask a question
  about it is less friction compared to the user attempting to describe it using
  natural language alone - humans want to point at a thing and ask questions
  about it.
  - Similar projects - Google's Stitch / Claude Design - right click on an
    element in the design and ask a question / add a comment about it.
- **Tooling**: The LLM must be able to control the UI as well as see it,
  visually feeding data back to the user by making changes - e.g. switching
  page, building queries, building node graphs and charts, and adding debug
  tracks. This is the other side of click-to-context: click-to-context feeds the
  user's view into the model, tooling feeds the model's actions back into the
  user's view.

![The Perfetto UI with the assistant sidebar open on the right: a chat
conversation sits alongside the timeline, with the current area selection
feeding the conversation as context.](media/0029/llm-sidebar.png)

_Prototype: the assistant sidebar alongside the timeline, answering questions
about the current selection. Note the context chips, tool use, and thought
presentation._

### Trust & reliability

RFC-0025 sets two quality principles this design must implement: make it
clear when something is a **trace-backed fact versus a model-generated
theory**, and keep the **underlying evidence inspectable**. They cut in both
directions:

- **Inputs**: the user must be able to see exactly what the model sees - the
  context strip in the chat UI, with its expand-to-raw-payload view, provides
  this (see Context Injection).
- **Outputs**: the transcript renders the model's work, not just its
  conclusions - every tool call and its result (the SQL that was run, the
  rows that came back) is visible inline, so any numeric claim can be traced
  to the query that produced it. A claim with no tool call
  behind it is visibly a theory. Tools like
  `add_debug_track` push this further: findings land on the timeline next to
  the evidence, where the user can verify them against the trace directly.

Finally, per RFC-0025's user-control default: the assistant is opt-in - it stays
out of the way (and sends nothing anywhere) until the user has explicitly
configured or accepted a provider, and we will have a prominent setting that
turns all AI assistant features off.

## Plumbing & Config

The plumbing and configuration will be similar to other LLM harnesses that
support multiple backends.

### Plugin structure

The feature is split across three kinds of plugins:

- `dev.perfetto.Llm`: Common LLM gateway plugin that all plugins requiring LLM
  services can use to access models. Defines settings to configure providers and
  models. This is kept separate from the assistant plugin so that other plugins
  may make use of LLMs outside of the assistant - e.g. auto generating summaries
  on details panels or in the overview page (though we may want to integrate
  this with the assistant anyway... TBD).
- `dev.perfetto.Intelletto`: The assistant plugin. Renders the assistant UI
  and handles tool calls, context injection, skills, etc. Other plugins can
  depend on and extend the assistant's functionality with additional tools,
  skills and context. Depends on `dev.perfetto.Llm`.
- `dev.perfetto.LlmProtocolXXX`: Backend implementations for the various
  endpoint APIs (e.g. Gemini, OpenAI, Anthropic, Prompt API, etc). Each
  depends on `dev.perfetto.Llm`.

How the pieces fit together:

```txt
          LLM backends (Gemini / OpenAI / Anthropic / ...)
                  ▲
                  │ HTTP
   ┌──────────────┴──────────────┐
   │ dev.perfetto.LlmProtocolXXX │  (one per backend API)
   └──────────────▲──────────────┘
                  │ registers a Protocol
   ┌──────────────┴──────────────┐   ┌───────────────────────┐
   │ dev.perfetto.Llm            │◄──│ other plugins         │
   │ (gateway: providers/models) │   │ (e.g. auto summaries) │
   └──────────────▲──────────────┘   └───────────────────────┘
                  │ "the conversational model, please"
   ┌──────────────┴──────────────┐   ┌───────────────────────┐
   │ dev.perfetto.Intelletto     │◄──│ other plugins         │
   │ (the assistant)             │   │ (tools, skills,       │
   └──────────────┬──────────────┘   │  context providers)   │
                  │ renders sidebar  └───────────────────────┘
                  ▼
   ┌────────────────────────────────────────┬──────────────┐
   │ Perfetto UI                            │ Assistant    │
   ├────────────────────────────────────────┼──────────────┤
   │ ▸ track A   ███ ▂▃▅█▅▃                 │ > why is     │
   │ ▸ track B      ██████                  │   this frame │
   │ ▸ track C   ██   ███   ██              │   janky?     │
   ├────────────────────────────────────────┤              │
   │ details panel                          │ Looking at   │
   │                                        │ the trace... │
   └──────────────┬─────────────────────────┴──────────────┘
                  ▲
                  │ WebSocket (tool calls round-trip to the UI)
   ┌──────────────┴──────────────┐       ┌──────────────────────┐
   │ trace_processor --httpd     │◄─MCP──│ external harness     │
   └─────────────────────────────┘       │ (e.g. Gemini CLI):   │
                                         │ same tools, skills & │
                                         │ context, sans the    │
                                         │ sidebar              │
                                         └──────────────────────┘
```

### The Protocol → Provider → Model stack

The configuration stack is broken into three layers.

A **Protocol** is code-behind implementing the hooks that describe how to talk
to a _kind_ of API (e.g. Gemini, OpenAI, Anthropic, Prompt API). Protocols are
provided by plugins, and one protocol can back many providers. A protocol must
be able to:

- **Create a stream**: take a neutral request (messages + system prompt + tool
  defs + model params) and return a streamed response, exposing at least
  incremental text, tool calls, and a finish/stop reason.
- **Translate tool definitions**: convert the gateway's neutral tool schema
  into the backend's native format (and down-convert the JSON Schema where the
  backend only accepts a subset, e.g. Gemini).
- **Translate tool use**: map native tool-call messages back to neutral
  `{name, args}`, and neutral tool results forward to the backend's native
  result format, preserving the call/result id threading.
- **Count/report tokens & errors**: surface usage and normalise backend errors
  (rate limit, auth, context-length) into a common shape.
- **Report a list of models**: makes the settings configuration a lot easier
  if you can see the list of models and their codenames.

A heavily simplified sketch of the Gemini protocol, to show the shape (real
implementation: SSE parsing, error normalisation, abort handling, and
Gemini's tool-call quirks omitted):

```ts
export class GeminiProtocol implements Protocol {
  readonly id = "gemini";
  readonly label = "Google Gemini";
  readonly capabilities = { nativeToolCalling: true, streaming: true };

  // Drives the credentials form in settings.
  readonly credentialFields = [
    { key: "apiKey", label: "API key", secret: true, required: true },
  ];

  async listModels(creds: Credentials): Promise<AvailableModel[]> {
    const resp = await fetch(`${ENDPOINT}/models?key=${creds.apiKey}`);
    const json = await resp.json();
    return json.models.map((m) => ({ name: m.name.replace(/^models\//, "") }));
  }

  // One neutral request in, one streamed turn out. The multi-step tool-use
  // loop lives in the consumer (the agent), not here.
  async *createStream(
    request: NeutralRequest,
    creds: Credentials,
  ): AsyncGenerator<StreamEvent> {
    const url =
      `${ENDPOINT}/models/${request.params.modelName}` +
      `:streamGenerateContent?key=${creds.apiKey}`;

    const resp = await fetch(url, {
      method: "POST",
      body: JSON.stringify({
        // Neutral -> native: messages, system prompt, tool defs.
        contents: messagesToContents(request.messages),
        systemInstruction: { parts: [{ text: request.systemPrompt }] },
        tools: toolsToDeclarations(request.tools),
      }),
    });

    // Native -> neutral: each streamed chunk becomes neutral events.
    for await (const part of streamParts(resp)) {
      if ("text" in part) {
        yield { type: "text", text: part.text };
      } else if ("functionCall" in part) {
        const { name, args } = part.functionCall;
        yield { type: "tool-call", call: { name, args } };
      }
    }
    yield { type: "stop", reason: "end" };
  }
}
```

A **Provider** is a data-only configured source: a reference to a protocol,
the connection details (API key, base URL etc.), and the catalog of **Models**
it exposes. Each model entry carries its backend model id, model params
(temperature, thinking mode - where applicable), an optional model-specific
system prompt, and its role(s) - e.g. 'conversational' or 'flash' - which
define where the model shows up: a plugin asking the core LLM plugin for a
model requests specific roles (the assistant only wants conversational
models). A provider may also define a preferred model per role, which lets the
assistant 'just work' for Googlers when the provider is pushed down by the
internal extension server. Providers can be supplied by extension servers, by
`embedders.ts`, or by end users via a setting (stored in localstorage).

Since a provider is pure data, it's just a JSON blob:

```json
{
  "id": "google-ai-studio",
  "label": "Google AI Studio",
  "protocol": "gemini",
  "credentials": {
    "apiKey": "AIzaSy..."
  },
  "models": [
    {
      "id": "gemini-2.5-pro",
      "label": "Gemini 2.5 Pro",
      "roles": ["conversational"],
      "params": { "temperature": 0.7, "thinking": true },
      "systemPrompt": "Prefer concise answers."
    },
    {
      "id": "gemini-2.5-flash",
      "label": "Gemini 2.5 Flash",
      "roles": ["flash"],
      "params": { "temperature": 0 }
    }
  ],
  "preferredModels": {
    "conversational": "gemini-2.5-pro",
    "flash": "gemini-2.5-flash"
  }
}
```

![The dev.perfetto.Llm settings panel: a default-model dropdown at the top,
then a list of configured providers - an OpenAI-compatible llama.cpp server
and a Google Gemini provider - each with credentials and a model list with
per-model role checkboxes.](media/0029/llm-settings.png)

_Prototype: the settings panel - providers, their model catalogs with role
tags, and the default model selected in one place._

The **Selected/Default Model** is the active `Provider:Model` for each role
type, owned by `dev.perfetto.Llm` as a user setting
(`dev.perfetto.Llm#SelectedModel`). It lives in the gateway rather than the
assistant precisely because the assistant is just one consumer: most plugins
just ask for the 'insert-role-here' model and use that one, so the model used
throughout the UI can be changed for all LLM users in a single place.

### Protocol implementation: build vs. library

The Protocol layer (normalizing wire formats, tool-call translation, streaming,
error handling across LLM backends) is a significant amount of work to implement
from scratch (though AI coding assistants make light work of stuff like this).
Rather than rolling our own, we _could_ leverage an existing open-source
TypeScript library.

Options considered:

- **Vercel AI SDK** (`@ai-sdk/provider` + `@ai-sdk/provider-utils`)
  - Pros: Mature, well-maintained, formal `LanguageModelV4` spec for custom
    providers; built-in providers for OpenAI, Anthropic, Google, Cohere,
    Mistral, Groq, etc.; handles streaming, tool calling, structured outputs,
    prompt caching; strong TypeScript types; active community.
  - Cons: Adds dependencies; provider registration is static at initialisation
    (no built-in runtime plugin registration — would need a thin wrapper
    layer); React-oriented UI layer (but AI SDK Core is framework-agnostic).
- **llm-harness** (TypeScript, Node.js)
  - Pros: Thin router with built-in retries, circuit breakers, fallback
    chains, cost tracking; peer dependencies (lazy-loaded, small bundle);
    supports OpenAI, Anthropic, Google, Ollama, and any OpenAI-compatible
    endpoint.
  - Cons: Smaller community; fewer provider implementations out of the box;
    no built-in plugin registration mechanism.

**Open question** - this is unresolved (see Open questions at the end). What
decides it: dependency/bundle footprint, whether the library can be wrapped
behind the plugin-registered Protocol interface (both options assume static
registration, so a wrapper layer is needed either way), and how much of the hard
part (streaming, tool-call quirks, error normalisation) it actually absorbs.
Either way the Protocol interface above is the contract; a library is an
implementation detail behind it, so the choice doesn't leak into the rest of the
design.

## Context Injection

This section describes the mechanism behind **click-to-context** (see UX
Design above). For a seamless integration with the UI - the model will need to
know what the user is currently looking at to provide more context around a
prompt - similar to how selecting lines of code in an IDE can provide crucial
context that would be laborious to try and describe in prose.

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
with the assistant plugin. A single `getContext()` callback returns both the
plain-language summary shown on the chip and the raw payload sent to the model -
one source of truth, so what the user sees and what the model receives cannot
drift apart (the trust property above). Returning `undefined` means "nothing
relevant right now": the chip disappears and nothing is sent. The harness owns
chip rendering, the include/exclude toggle, and the expand-to-raw-payload view;
the provider only supplies content.

For example:

```ts
assistantPlugin.registerContextProvider({
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
**system prompt** rather than repeating it alongside every payload:

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

### Text-first: when images are appropriate

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
  is out of scope here beyond noting the one plumbing prerequisite: the
  Protocol layer's neutral request format must be able to carry image parts in
  messages and tool results (all major backend APIs support this).
- **Images pasted from outside Perfetto** - a monitoring dashboard, a bug
  report screenshot, an architecture diagram: "here's the latency spike our
  monitoring caught at 14:32, find what caused it in this trace". The image
  carries context that lives nowhere in the trace tables, so there is no SQL
  alternative. Also punted, same prerequisite.

In short: images are accepted when they carry evidence the trace tables don't
contain, not as a substitute for querying data we already have.

## Tools

Tools are how the model interacts with the trace and the UI: run queries, build
Data Explorer graphs, select events, switch pages, add debug tracks, etc. Tools
are provided by plugins (a tool is ultimately a callback) and registered with
the assistant plugin, the same way as context providers.

**Open question - how tools relate to commands.** The UI already has a
plugin-registered name + callback mechanism (commands), and this section
describes two ways a tool surface could be built on or alongside it. The main
problem with commands is they cannot be used as tools verbatim, as tools require
an input schema and a description on how and when to use them, which commands
currently lack.

Comments on which option to pursue are explicitly welcome:

1. **Tools are their own thing**: a standalone `registerTool` API, separate
   from commands (see 'Registering a tool' below). Simple and self-contained;
   the cost is a second registry that overlaps with commands wherever a
   capability should be both user-invokable and model-callable.
2. **Commands grow an optional tool-like interface**: commands gain optional
   descriptions and typed input schemas at their registration site, and an
   allowlist decides which are exposed to the model (see 'Relationship to
   commands' below). One registry, no duplication - but it couples the
   command API to model-facing concerns.

The likely landing point is a hybrid - schema'd commands for capabilities
that are meaningful in both worlds, plus standalone tools for model-only
surface (`get_schema`, `list_skills`) - but where the line sits is
unresolved; both subsections below should be read as candidate designs, not
settled ones.

**Open question**: whether tool registration lives in the llm plugin or the
assistant plugin - other users of the llm plugin may or may not want to use
tools.

### Registering a tool

This is option 1's shape (and what the model-only tools need under either
option). A command is just a name + callback; a tool needs more, because the
model has to _decide_ to call it and call it _correctly_, and the harness has
to validate, execute, and serialise the result. For example:

```ts
assistantPlugin.registerTool({
  // Stable, model-API-safe identifier (typically ^[a-zA-Z0-9_-]+$, so dotted
  // plugin-style ids need normalising).
  name: "run_query",

  // Tells the model *when* to call the tool, not just what it does - most
  // per-tool tuning effort lands here. "Run a
  // PerfettoSQL query" is weak; the version below actually drives correct
  // tool selection.
  description: `Query trace data when the user asks about durations, counts,
or relationships between events. Prefer aggregation (COUNT/GROUP BY) over
pulling raw rows. Returns at most ${MAX_ROWS} rows.`,

  // Authored in Zod, not hand-written JSON Schema. One declaration buys three
  // things: TS types for the callback args, the JSON Schema sent on the wire
  // (down-converted per backend by the Protocol layer), and runtime
  // validation of the model's args. Field descriptions matter - the model
  // reads those too.
  inputSchema: z.object({
    sql: z.string().describe("The PerfettoSQL query to run."),
  }),

  // Metadata the harness keys off: read-only vs. mutating (what a future
  // consent model would gate on), and which environment(s) the tool is valid
  // in (mirrors the skills environment tagging).
  kind: "readonly",

  // Args arrive typed and pre-validated. The harness serialises the return
  // value into the tool-result string.
  async callback({ sql }) {
    const res = await trace.engine.query(capRows(sql, MAX_ROWS));
    return serializeResult(res);
  },
});
```

The model will eventually emit malformed args; validating against the schema up
front turns that into a clean tool-result error the model can self-correct
from, rather than an exception in plugin code. On the output side the harness
just serialises the return value - everything about result size lives in
'Managing huge tool outputs' below.

### Example tool surface

- `run_query(sql)` - run PerfettoSQL. The main output-size offender.
- `get_schema(table?)` - list tables/columns so the model can write valid SQL
  without the whole schema living in the system prompt.
- `get_selection()` - read the current timeline/DE selection (read-only).
- `select_timeline(...)` / `select_area(range)` - drive the timeline
  (mutating).
- `create_de_graph(...)`, `navigate(page)` - build a DE node, switch page.
- `add_debug_track(sql, ...)` - visualise a query result as a debug track on
  the timeline (mutating). A natural way for the model to show its findings
  in-situ - "here are the long frames" lands better as a track alongside the
  evidence than as a list in the chat.

Note the split: read tools return data (and hit the size problem below);
mutating tools just ack/error.

### Relationship to commands

Commands fall into roughly three non-mutually-exclusive types: palette
commands, model-callable commands (tools), and commands stable enough for
startup commands and macros.

This is option 2: rather than a separate tool registry, commands themselves
gain what the model needs. It splits the problem into **contract** and
**policy**:

- **Contract at the registration site**: commands gain _optional_ metadata -
  a description and a typed input schema - declared where the command is
  registered. This keeps the drift-prone part (a schema coupled to the
  callback's actual arguments) next to the implementation, and it pays off
  beyond the assistant: a schema'd command benefits the palette (argument
  prompting), startup commands, macros, and the future UI RPC protocol
  whether or not it's ever exposed as a tool.
- **Policy in an allowlist**: whether a command is exposed to the model is
  decided by an allowlist, mirroring exactly how startup commands are
  designated today. The tool surface directly affects model behaviour (too
  many tools or weak descriptions degrade tool selection), so it's better to
  answer "what can the model call" in one place than across every plugin's
  registration calls.

The same policy split should apply in the other direction: **palette
visibility becomes an allowlist too**. Under this option, capabilities that
only make sense for the model (`get_schema`, `list_skills`) are still
registered as commands, and without curation they'd clutter the palette with
entries no human would ever invoke. Each surface - palette, model, startup
commands/macros - selects from the one command registry via its own
allowlist, rather than every command appearing everywhere by default.

Two rules keep the allowlist honest:

- Allowlisting a command that lacks a description or input schema is a
  **hard error at startup** - the allowlist cannot silently drift against
  the registrations.
- The allowlist is the assistant plugin's registry, not a static file: core
  ships its entries, and third-party plugins contribute their own command
  ids through the same API at runtime.

(In the hybrid landing point from the open question above, model-only
capabilities would instead use the standalone `registerTool` API and never
enter the command registry - the palette allowlist is then only needed for
edge cases.) Commands gaining schemas is also the first step the UI RPC
protocol below needs.

### Managing huge tool outputs

A trace query can return millions of rows:

- **Hard row cap** on `run_query` (wrap as `SELECT * FROM (<sql>) LIMIT N`) so
  a stray `SELECT *` can't blow the context.
- **Truncate with an explicit marker** ("... N more rows truncated") so the
  model knows the result is partial and doesn't reason over it as complete.
- **Steer toward aggregation**: tool descriptions + a skill push the model to
  `COUNT/GROUP BY/LIMIT` rather than pulling raw rows.
- For genuinely large results, **return a handle + summary** (row count,
  columns, sample rows) - the result stays in TP (e.g. as a DE node) and the
  model drills down with follow-up queries.

Longer term, output handling may grow beyond "serialise to string": whether a
tool returns data vs. just acks (the read/mutating split), and whether a
large result comes back as a handle + summary rather than inline, is
output-shape metadata on the tool definition that the harness can branch on.

### No consent model

Unlike a coding agent, there is no per-action confirmation or allowlist - the
model may call any registered tool at any time. The tools are non-destructive, all operate on a trace the user already has open,
and nothing is persisted outside the session, so the worst case is a wasted
query or an unexpected navigation - trivially undone. There is no per-tool
"destructive" confirmation gate, because no Phase 1 tool warrants one.
Reconsider if a tool ever gains side effects that escape the session (writes to
disk, network calls, mutating shared state) - the `kind` metadata above is the
natural hook to gate on then.

### Dynamic (lazy) tool loading

A tool is only callable if its definition is in the request's tool list, so
rather than dumping every definition into every request, we start with a
minimal bootstrap set - `list_tools` plus `more_tools(names)` - and grow the
list as the model asks: `list_tools` returns names + descriptions (cheap);
`more_tools(["run_query", ...])` makes the harness add those definitions to the
tool list on the _next_ request, where they stay for the rest of the turn.

- This is purely a harness concern - we own the tool list on every request -
  so it's provider-agnostic: a client-side equivalent of provider-native "tool
  search" that works against any backend (Gemini, openai-compatible, local
  models). Where a provider offers native tool search, that becomes an
  optional optimisation, not a dependency.
- Append-only, for the cache: tool definitions sit at the front of the prompt,
  so removing or reordering one busts the whole cache prefix. Only ever grow
  the list within a turn, appending to the end.

## Agent loops

The model runs a call → execute tool → feed result back → repeat loop until it
produces a final answer with no further tool calls. The loop lives in the
consumer - the LLM gateway stays a single request/response. The only bound is a
cap on iterations per user turn, which stops runaway loops and caps token
spend; hitting it surfaces to the user rather than failing silently.

Errors are routed to one of two destinations:

- **Tool errors go to the model** as the tool result (e.g. a SQL syntax error)
  so it can self-correct and retry, bounded by the iteration cap.
- **Backend and loop errors go to the user**: rate limit, auth failure, model
  unreachable, context-length exceeded, iteration cap hit. These render inline
  in the chat window - the chat is the single transcript of everything that
  happened, successes and failures alike, so the user always sees why a turn
  stopped. The Protocol already normalises backend errors into a common shape
  (see Plumbing); the chat just renders it.

Cancellation & steering - because the loop can fire many tool calls, the user
needs a way to intervene:

- **Cancel** (Phase 1): abort the in-flight request (`AbortController`) and
  stop the loop from starting the next iteration. Keep what completed, marked
  interrupted, so the transcript stays truthful.
- **Queued follow-up** (Phase 1, cheap): a message typed mid-loop is enqueued
  and picked up at the next turn boundary - one check at the top of the loop,
  no interruption machinery. Covers most of what users want ("oh wait, also
  check X").
- **True mid-flight steering** (deferred): injecting the message _into_ the
  running loop needs safe interruption points and in-flight tool-call handling
  - real machinery, not worth it while cancel + queued follow-up cover the
    common cases.

Cost / token visibility: the Protocol already reports token usage per request
(see Plumbing), so we can surface running usage to the user - tokens (and,
where the provider gives a price, cost) per turn and per conversation. Matters
for anyone on a per-token plan or a quota, especially since the agent can loop
and spend more than a single round-trip would suggest. Probably a lightweight
indicator rather than Phase 1 core; noting it so the usage the Protocol
surfaces doesn't go unused.

## Skills

Skills fall into roughly three camps:

1. Skills that teach the model how to use the UI (including referencing tools).
2. Skills that teach the model how to diagnose issues agnostic of the UI.
3. Skills that assume we're running from the command line - oriented toward
   traditional terminal-based AI harnesses that can shell out to other tools.

The UI can only make use of the first two, and TP users won't be able to use the
UI skills. Skills should therefore be tagged with the environment they're
limited to, so the UI can make the first two camps available to the model
without being misled by the third, and vice versa for the TP-level skills.

Rather than a single environment enum, the tag is a **capability requirements
list**: each skill declares what it needs from the host, and a host declares
what it offers; a skill is available iff its requirements are a subset of the
host's capabilities. Initial capability values:

- `ui` - the skill references UI tools or workflows (navigation, Data
  Explorer, selection). Offered by the in-panel assistant; not by a terminal
  harness talking to bare TP.
- `shell` - the skill assumes it can shell out to other tools. Offered by
  terminal harnesses (Claude Code etc.); never by the UI.

Camp 2 (UI-agnostic diagnosis) falls out naturally as `requires: []` - usable
everywhere, since trace query access is the baseline every host offers. The
list extends without re-tagging existing skills when a new capability appears
(e.g. a future `data_explorer` requirement if DE tools become optional).

Right now, skills are not built into the UI - they just live in our repository
as markdown. They will need to be packaged with their requirement tags into a
registry the assistant plugin can enumerate at runtime, and exposed to external
harnesses in their native format (e.g. as installable Claude Code skills).

Ingesting skills - how the model gets skill content into context - presents
the same dump-vs-lazy-load tradeoff as tools, but the constraint differs: a
skill is just prompt text, with no "must be in the request to be callable"
rule, so either approach works. Dumping them all into the system prompt is
simplest, but every skill's full body then sits in context every turn whether
relevant or not, costing tokens and bloating the cache-stable prefix.
Lazy-loading via a `list_skills` tool - returning just names and descriptions
up front and letting the model pull a skill's full body on demand - is the
same shape as `list_tools`/`more_tools` for tools (see Dynamic tool loading
above) and is preferred for the same reason: keep the per-turn context small
and load detail only when the task calls for it. Tool ingestion is already
resolved this way; skills follow the same pattern.

### Self-help

Users should be able to ask the assistant how to use the Perfetto UI as a whole,
including how to use the assistant itself. "How do I find the longest-running
slice?", "what can the Data Explorer do?", "how do I ask you to compare two
traces?", "What tools do you have available?", "Where is my data sent when I
use you", should all get useful answers. This is the substrate the **Teaching**
UX property (see above) is built on: the assistant can only teach the UI if it
knows the UI.

The same corpus also handles **model bootstrapping** - the model reading up on
its own harness so it knows what it can do before it tries to do it. A common
failure mode of LLM harnesses is that the model has no idea what the harness
it's running inside can actually do - so it invents features, misses real
ones, and can't answer "how do I..." questions about itself. Both needs are
served by the same docs.

The mechanism is a combination of the minimal system prompt and a
`read_docs(topic?)` tool (or similar) that lets the assistant pull documentation
about Intelletto and the Perfetto UI on demand - what the UI can do, the
conventions for driving it, what tools and skills exist and how they compose.
Without a topic it returns an index; with one it returns that page. (Tool
_discovery_ - how the model finds and loads callable tools - is a separate
mechanism; see Dynamic tool loading above.) Prior art: the pi coding harness
ships a read-docs tool that lets the model read its own documentation on demand,
exactly the pattern we want here.

This is a tool rather than part of the system prompt: the corpus can be large
and is mostly irrelevant on any given turn, so pulling it on demand keeps it
out of the cache-stable prefix (see prompt ordering below) and out of the
context until the assistant actually needs it.

The docs should be authored as content (not generated solely from tool
schemas) so they can carry the _why_ and the worked examples - how to chain
`get_schema` -> `run_query` -> `create_de_graph`, when to aggregate vs. pull
rows, etc. - the things a bare schema can't convey. For the source of truth,
the same skill/tool registries that back the discovery tools should back this,
so the docs can't drift from what's actually registered. A tool's description
and a skill's body are the docs; `read_docs` just surfaces them plus any
hand-written overview pages.

## System prompt

The assistant plugin owns system prompt assembly. There is **no free-text
"extend the system prompt" hook for plugins**: the system prompt is sent with
every request of every conversation and sits in the cache-stable prefix, so an
open hook invites every plugin to dump always-on prose there.
Instead, prompt-worthy content is colocated with the artifact that needs it,
and the assembled prompt concatenates:

- **Application brief** - the fixed prompt of whichever consumer is driving
  the gateway; the conversational assistant's ("you are in a trace viewer tool
  called Perfetto and your job is to help diagnose issues by looking at a
  trace") differs from e.g. a trace summariser's.
- **Model-specific prompt** - from the provider/model config (see Plumbing),
  for per-model fine tuning (e.g. "Don't mention goblins").
- **Context provider `description`s** - payload format explanations, included
  while the provider is registered (see Context Injection above).
- **Tool descriptions** - when-to-call-me guidance, carried by the tool
  definitions themselves.
- **Skill index** - names + descriptions; full bodies load lazily (see
  Skills).
- **User instructions** - a settings textbox for end users ("my app's main
  thread is called WorkerPool-3", "show me queries before running them") -
  appended last so the harness's own invariants stay authoritative.

Assembly rules, regardless of source:

- Contributions must be invariant for the conversation - no volatile content
  (no selection state, no timestamps-of-now). Anything per-turn belongs in a
  context provider payload instead.
- The harness concatenates contributions in a stable order (e.g. sorted by id)
  so the assembled prompt is byte-identical across turns and the cached prefix
  survives. Registration after a conversation has started takes effect on the
  next conversation, to avoid busting the cache.

Note that tool definitions themselves are not injected by us manually in to the
user turn: they are submitted via the API's native tool-definition channel with
each request, and however the backend folds them into its prompt is its concern,
we just supply the tool list (see Tools).

## Conversation & context management

**Conversation state.** LLM endpoints are stateless - the server keeps no
conversation, so the full message history (system prompt + every prior turn +
tool calls/results) is resent on every request. The client owns the
conversation, so we must hold it ourselves. Prompt ordering matters for
caching: backends (and local models) cache by matching prompt
prefix, so keep stable content (system prompt, skills, tool defs) at the front
and volatile content (live UI prompt-context) at the end, or we bust the cache
every turn.

**Conversation lifetime & scope.** A conversation is scoped to a single trace,
and kept in memory only. When the trace is reloaded or replaced - or the tab
is closed - the conversation is forgotten. We do **not** persist history to
local storage or anywhere else for now; the user can also reset the chat
history manually, and once it's gone it's gone.

We considered letting conversations outlive the trace (registered in `onActivate`, so
context could carry across traces) - but context gathered against one trace
(selections, query results, "the stall at frame 43") is largely nonsensical
against a different trace, so carrying it over would mislead more than it
helps. Scoping to a single trace sidesteps that, and dodges the harder
questions (where history is stored, how it's keyed to a trace, stale-context
handling) until there's a reason to solve them.

The cost is that a page reload or accidental tab close loses the
investigation. If that proves painful, revisit with per-trace persistence
keyed on a stable trace identity - but that's explicitly out of scope here.

Context compaction: initially none - it's a complex and destructive feature.
Once users reach their context window limit (as defined by an error from the
server), the assistant will simply stop working and the user will have to open a
new conversation.

We will provide an export button to allow users to export their conversation to
another tool if they so desire.

## External agents via `trace_processor` as a conduit

Most of this doc is about the in-panel assistant, but the same tool surface
should also be reachable by **external coding agents** (Gemini CLI, Claude Code,
Codex, Cursor, etc.). None of this is Phase 1 work: the bridge and all
external-harness support is punted to Phase 2 at the earliest (see Roadmap);
it's designed here so the tool and skill contracts don't have to change to
accommodate it later.

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
  `<ui_context>` block becomes a `get_ui_context()` tool returning exactly
  what the context providers would have emitted. The model has to _decide_ to
  call it - steered via the server instructions ("call `get_ui_context`
  before answering questions about 'this'") - which is strictly worse: the
  user clicks a slice and the external agent doesn't know unless it asks.
  Click-to-context largely does not survive the bare conduit - this gap is
  the main reason the embedded assistant exists.
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

## Roadmap

**Status: a working prototype exists.** The screenshots in this doc are taken
from it; it covers most of Phase 1 (the Protocol/Provider/Model stack with
Gemini and OpenAI-compatible protocols, the sidebar with context chips and
tool use, the core tool surface). Phase 1 is therefore largely a landing
plan - review, hardening, and upstreaming existing code - rather than
speculative work, and the design above is informed by it rather than
hypothetical.

See: https://github.com/google/perfetto/pull/6209

- **Phase 1 — core plumbing** (mostly prototyped): Protocol/Provider/Model
  config layers, Gemini protocol, agent sidebar, basic context injection
  (page + selection + viewport), core tool surface (SQL queries, selection,
  timeline navigation, Data Explorer state). Embedded assistant only - no
  bridge or external-harness support of any kind. Done when: a user with a
  configured provider can run the intro's example prompts end-to-end in the
  sidebar.
- **Phase 2 — richer context & extensibility**: click-to-context on anything,
  additional protocols, tools from other plugins, skills integration,
  external agent conduit via TP, merge with and deprecate PerfettoMcp. Done
  when: a third-party plugin can register a tool, a skill, and a context
  provider without core changes, and an external harness can drive the UI
  through the TP conduit.
- **Phase 3 — advanced**: context compaction, more provider types, extension
  server integration, first-party extensions for external harnesses (if
  warranted), richer tool surface as plugins add their own.

### Migration from `com.google.PerfettoMcp`

`PerfettoMcp` already does a version of this - exposing the trace to an LLM -
but in a more limited way. Rather than running two overlapping mechanisms,
the plan is to fold its useful pieces into this design and **deprecate
PerfettoMcp** once the assistant's tool surface covers what it did (a Phase 2
step above, alongside the external agent conduit it overlaps with).

## Open questions

Repeated from the sections above - these are the places where reviewer input is
most wanted:

1. **Build vs. library for the Protocol layer** (see Plumbing & Config):
   roll our own backend normalisation or wrap an existing SDK (Vercel AI SDK,
   llm-harness). Contained behind the Protocol interface either way.
2. **Tools vs. commands** (see Tools): whether tools are their own
   registration mechanism, or commands grow optional descriptions + input
   schemas with an allowlist deciding model exposure - or where the hybrid
   line sits between the two.
3. **Where tool registration lives** (see Tools): the LLM gateway plugin or
   the assistant plugin - other gateway consumers may not want the
   assistant's tool semantics.
