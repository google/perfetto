# Perfetto UI: Intelletto - AI Assistant

**Authors:** @stevegolton

**Status:** Draft

## Introduction

> **Note on the name.** _Intelletto_ is a **codename** for the assistant, used
> in the plugin id (`dev.perfetto.Intelletto`); this doc otherwise just says
> "the assistant". The name may be subject to change.

This doc proposes a new AI assistant feature in the Perfetto UI which
leverages the power of LLMs to let users interact with the trace using natural
language - for example:

- Answering questions about their trace.
- Writing queries.
- Building node graphs in Data Explorer.
- Navigating the trace, selecting events.
- Teaching users how to use the UI.

The real value-add of embedding an assistant in the UI specifically - as
opposed to a 3rd party harness connected to TraceProcessor - is the additional
context a visual interface can provide to the model. Rather than attempting to
describe what they want in words, the user can click on elements in the UI and
ask pointed questions, and the model has enough information to understand the
context.

There is prior art in both directions. Within the codebase,
`com.google.PerfettoMcp` already offers similar but simpler chatbot-style
functionality, and will likely be replaced by the tool described in this doc
(see the dedicated section near the end). More broadly,
[RFC-0025](https://github.com/google/perfetto/discussions/5763) describes how
to integrate AI tools into Perfetto in general; this doc extends that
discussion with concrete suggestions for integrating a conversational agent
into the UI - for motivation, see that doc.

### Scope

This RFC covers the implementation in the OSS codebase - the provider-agnostic
plumbing. It makes no judgement about which backends are used or what data is
acceptable to send to them: trace contents are sent to whichever endpoint the
user configures, and any data-egress / privacy policy is a deployment concern
layered on top, out of scope here. Likewise, API key handling is up to the
user or specific deployment.

This RFC also doesn't cover classic ML models for uses such as classifying
traces. While the assistant could certainly make use of ML-powered tools in
the future, this doc is focused on the assistant, which will leverage LLMs.

### Text-first: where images do and don't earn their place

The assistant is text-first: context and tool results are textual, and the aim
over time is to make more of the UI expose its data in a form a text model can
ingest.

In particular, **screenshotting Perfetto's own rendering of queryable data**
(the timeline, a DE table) and feeding it to the model is explicitly rejected.
It is tempting because it needs no plumbing and demos well on any view, but it
loses to text on every axis that matters:

- It's a lossy downsample of data we already hold losslessly: a timeline pixel
  column can represent thousands of culled, overlapping slices, while
  `run_query` returns the same underlying data with full precision at a
  fraction of the token cost.
- Models misread charts confidently - plausible-but-wrong numbers read off
  pixels are close to disqualifying in a measurement tool, whereas a SQL
  result is either right or visibly errors.
- It's fragile (coupled to theme, zoom, viewport, DPI) and creates no reusable
  capability - effort spent on screenshot ingestion is effort not spent making
  components expose their data textually, which serves chips, tools, and
  external agents alike.

Image input _is_ in scope where the pixels carry information the trace tables
don't:

- **Images embedded in the trace itself** - some traces capture screenshots /
  framebuffers / layer snapshots (e.g. Android's screenshots track, surface
  captures). These are first-class trace artifacts recording what was actually
  on screen at time T - "the frame was delivered but the screenshot shows it
  was blank" is unrecoverable by SQL. Exposing these fits the existing design
  with no new concepts: a `get_screenshot(ts)`-style tool, and/or a context
  provider triggered by selecting a screenshot slice. **Implementation is
  deliberately punted** beyond noting the one plumbing prerequisite: the
  Protocol layer's neutral request format must be able to carry image parts in
  messages and tool results (all major backend APIs support this).
- **Images pasted from outside Perfetto** - a monitoring dashboard, a bug
  report screenshot, an architecture diagram: "here's the latency spike our
  monitoring caught at 14:32, find what caused it in this trace". The image
  carries context that lives nowhere in the trace tables, so there is no SQL
  alternative. Also punted, same prerequisite.

So the rule is: images are welcome when they are evidence the trace tables
don't contain; they are rejected as a substitute for querying data we already
have.

## UX

The UX of the assistant and how users interact with it will provide the main
advantage of using an embedded assistant tool over something like a 3rd party
harness connected to TraceProcessor. This is the real value-add that comes from
embedding this assistant in the UI.

### UX Requirements

- **Natural Language Prompting**: The user can interact with the assistant
  using natural language.
- **Conversational (multi-turn context accumulation)**: Investigations are
  rarely solved one-shot. Users should be able to ask follow-up questions,
  leaning on the combined context from any previous questions and responses.
- **Context travels with the user**: Context should not be tied to a given
  page, panel or element. The user must be able to navigate the UI, bringing
  any existing chat context with them, asking followup questions while
  bringing new context into the conversation from other parts of the UI.
- **Rich context injection:**: The model should be delivered relevant
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
  themselves, rather than silently doing it for them. Perfetto's UI is deep
  and much of its power is undiscovered; the assistant is well placed to
  surface the relevant feature, query, or workflow and explain it ("you can
  do this yourself with Data Explorer like so..."). This is in deliberate
  tension with Agency - do it for them vs. teach them to do it - and the
  right balance depends on the user and the task. Worth designing for both
  modes rather than assuming the assistant should always just act.

### UX Design

The assistant's UX surface is centered around three main concepts:

- **Chat in a sidebar**: This is a very common UX found in lots of AI tools, and
  for a good reason: it presents the chat history on one place and provides a
  persistent single always available prompt box which can be used to ask
  followup questions given the current context. Being in a sidebar means it
  follows the user around the UI even through page flips. It should be an
  obvious UX for anyone who's used an AI powered tool before.
- **Context injection**: The user should be able to click on UI elements using
  the mouse and bring them into the context. This is a very natural way to
  interact with a UI, and being able to click on something then ask a question
  about it in natural language provides a lot more context to the model compared
  to the user attempting to describe it using natural language. Humans want to
  point at a thing and ask questions about it.
  - Similar projects - Google's Stitch / Claude Design - right click on an
    element in the design and ask a question. The comment is ultimately added
    to the conversation in the sidebar.
- **Tooling**: On the other hand, the LLM must be able to control the UI,
  visually feeding data back to the user by making changes to the UI - e.g.
  switching page, building queries, building node graphs. This is the other side
  of context injection: context injection feeds the user's view into the model,
  tooling feeds the model's actions back into the user's view. Together the two
  close the loop - the user points, the model acts, and both are always looking
  at the same thing.

### Implementation

In an ideal world, we could click on any UI component and bring it into the
LLM. We're not there yet, but there is low hanging fruit that already exists:

- Current page
- Timeline selection
- Pinned tracks
- Currently selected node in the nodegraph
- Selected SQL code in the query page

Most of this information is already available on the global trace object, and
what isn't could easily be wired up. Making every UI element ingestible is a
massive job, so the initial phase focuses on exposing what we have; the
mechanism is described later in this doc in the 'Context Injection' section.

## Plumbing & Config

The plumbing and configuration will be similar to other coding harnesses that
support multiple backends.

### Plugin structure

The feature is split across three kinds of plugin:

- `dev.perfetto.Llm`: Common LLM/GenAI gateway plugin that all plugins
  requiring LLM services can use to access models. Defines settings to
  configure providers and models. This is kept separate from the assistant
  plugin so that other plugins can use the LLM backend - e.g. auto summaries
  on details panels.
- `dev.perfetto.Intelletto`: The assistant plugin. Renders the assistant UI
  and handles tool calls, context injection, skills, etc. Other plugins can
  depend on and extend the assistant's functionality with additional tools,
  skills and context. Depends on `dev.perfetto.Llm`.
- `dev.perfetto.LlmProtocolXXX`: Backend implementations for the various
  endpoint APIs (e.g. Gemini, OpenAI, Anthropic, Prompt API, etc). Each
  depends on `dev.perfetto.Llm`.

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
  if you can see the list of models and their code-names.

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

The **Selected/Default Model** is the active `Provider:Model` for each role
type, stored in the core LLM plugin as a user setting. Most plugins just ask
for the 'insert-role-here' model and use that one, so the model used
throughout the UI can be changed for all LLM users in a single place.

### Protocol implementation: build vs. library

The Protocol layer (normalizing wire formats, tool-call translation,
streaming, error handling across LLM backends) is a significant amount of work
to implement from scratch. Rather than rolling our own, we could leverage an
existing open-source TypeScript library. Options considered:

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

## Context Injection

For a seamless integration with the UI - the model will need to know what the
user is currently looking at to provide more context around a prompt - similar
to how selecting lines of code in an IDE can provide crucial context that
would be laborious to try and describe in prose.

To solve this problem, contextual information is serialized and appended to each
and every user turn prompt. Each bit of context is intentionally small and thus
is appended to every single prompt without worrying about blowing out the
context window. If we do need to expose larger pieces of information to the
model - expose it via a tool.

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
sidebar above the prompt input box. An assistant that silently sees things the
user can't is a trust and usability problem. The chat window has a **context
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
- If a page has no selection or no context provider, the strip just shows
  the page name; it's never empty and never silent about what's being sent.

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

Often the payload alone isn't self-explanatory - the model can't infer units,
what an id joins against, or which tool accepts which field. That invariant
explanation is supplied via the provider's optional `description`, which the
harness folds into the **system prompt** rather than repeating it alongside
every payload:

- **Sent once, not per turn** - it lands in the cache-stable prefix, while the
  per-turn `<ui_context>` block stays data-only. Anything that changes per turn
  belongs in the payload, not the description.
- **Included only while the provider is registered** - no timeline plugin
  loaded, no timeline payload explanation burning prefix tokens. (Keyed on
  registration, not on whether `getContext()` currently returns data - the
  latter changes per click and would bust the cache.)
- **Colocated with the payload it describes** - registered and unregistered
  together, written next to the code that builds `data`, so the explanation
  can't be orphaned or drift out of date.

## Tools

Tools are how the model interacts with the trace and the UI: run queries, build
Data Explorer graphs, select events, switch pages. Tools are provided by
plugins (a tool is ultimately a callback) and registered with the assistant
plugin, the same way as context providers.

**Open question**: whether tool registration lives in the llm plugin or the
assistant plugin - users of the llm plugin might not use tools the same way.

### Registering a tool

A command is just a name + callback; a tool needs more, because the model has
to _decide_ to call it and call it _correctly_, and the harness has to
validate, execute, and serialise the result. For example:

```ts
assistantPlugin.registerTool({
  // Stable, model-API-safe identifier (typically ^[a-zA-Z0-9_-]+$, so dotted
  // plugin-style ids need normalising).
  name: "run_query",

  // Tells the model *when* to call the tool, not just what it does - this is
  // load-bearing, and most per-tool tuning effort lands here. "Run a
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
from, rather than an exception in plugin code.

Output handling may grow beyond "serialise to string": whether a tool returns
data vs. just acks (the read/mutating split), and whether a large result comes
back as a handle + summary rather than inline (see huge outputs below), is
output-shape metadata the harness can branch on.

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

The proposal splits the problem into **contract** and **policy**:

- **Contract at the registration site**: commands gain _optional_ metadata -
  a description and a typed input schema - declared where the command is
  registered. This keeps the drift-prone part (a schema coupled to the
  callback's actual arguments) next to the implementation, and it pays off
  beyond the assistant: a schema'd command benefits the palette (argument
  prompting), startup commands, macros, and the future UI RPC protocol
  whether or not it's ever exposed as a tool.
- **Policy in a whitelist**: whether a command is exposed to the model is
  decided by a whitelist, mirroring exactly how startup commands are
  designated today. This is deliberate curation: the tool surface directly
  affects model behaviour (too many tools or weak descriptions degrade tool
  selection), so one place answering "what can the model call" beats
  exposure decisions smeared across every plugin's registration calls.

Two rules make the split safe:

- Whitelisting a command that lacks a description or input schema is a
  **hard error at startup** - the whitelist cannot silently drift against
  the registrations.
- The whitelist is the assistant plugin's registry, not a static file: core
  ships its entries, and third-party plugins contribute their own command
  ids through the same API at runtime.

The standalone `registerTool` API remains for model-only tools that make no
sense in the palette (`get_schema`, `list_skills`). Commands gaining schemas
is also the small version of a much bigger idea - it is exactly the first
step the UI RPC protocol below needs.

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
  model drills down with follow-up queries. This is the scalable answer;
  truncation is just the safety net.

### No consent model (deliberate)

Unlike a coding agent, there is no per-action confirmation or allowlist - the
model may call any registered tool at any time. This is a conscious choice: the
tools are non-destructive, all operate on a trace the user already has open,
and nothing is persisted outside the session, so the worst case is a wasted
query or an unexpected navigation - trivially undone. There is deliberately no
per-tool "destructive" confirmation gate, because no Phase 1 tool warrants one.
Reconsider if a tool ever gains side effects that escape the session (writes to
disk, network calls, mutating shared state) - the `kind` metadata above is the
natural hook to gate on then.

### Dynamic (lazy) tool loading

A tool is only callable if its definition is in the request's tool list, so
rather than dumping every definition into every request, we start with a
minimal bootstrap set - `list_tools` plus `more_tools(names)` - and grow the
list as the model asks: `list_tools` returns names + descriptions (cheap);
`more_tools(["run_query", ...])` makes the harness add those definitions to the
tool list on the _next_ request, where they stay for the rest of the turn. The
model never calls a tool that isn't there - it asks, we provision, it calls on
the next round-trip.

- This is purely a harness concern - we own the tool list on every request -
  so it's provider-agnostic: a client-side equivalent of provider-native "tool
  search" that works against any backend (Gemini, openai-compatible, local
  models). Where a provider offers native tool search, that becomes an
  optional optimisation, not a dependency.
- Append-only, for the cache: tool definitions sit at the front of the prompt,
  so removing or reordering one busts the whole cache prefix. Only ever grow
  the list within a turn, appending to the end.

### A UI RPC protocol (DevTools-for-Perfetto) - strategic option

The bigger picture behind "how does a command become a tool": give the UI a
proper **RPC channel** - a pipe carrying only serialisable JSON in both
directions - the equivalent of Chrome's DevTools Protocol, but for the Perfetto
UI. It would subsume both the assistant's tool surface and the omnibox/command
entry points, and massively boost integration with external AI agents and
automation generally. Commands are already half of this protocol if you squint:
add an input schema, schema'd JSON output, and marshal them over a WebSocket or
`postMessage` channel. The command-to-tool work above is the first concrete
step toward it.

- It must **not** subsume the TS plugin API. An RPC surface is a deliberately
  worse _programming_ model - procedure calls passing string ids, no live
  object graph - and that's exactly what makes it simple and stable to
  maintain. Plugins keep today's ergonomic `App`/`Trace` TS API for writing UI
  code; the RPC layer is for driving the UI from outside.
- Aspirational end state: **all UI actions that map to a command route through
  the command layer** (clicking a slice becomes `SelectSlice(trackUri,
sliceId)`). That buys: a "debugger" panel that snoops and replays the message
  stream; a goldmine of training data (real action sequences mapped to intent,
  if power users opt in); and one surface for many drivers - in-panel
  assistant, external agents, test harnesses, macros.
- **Open question (deserves its own doc)**: only a subset of the plugin API is
  naturally command-shaped, so the TS API and the RPC layer must coexist -
  where the boundary sits, and how actions get reflected into the command layer
  without forcing everything through it, is unresolved.

## Agent loops

The model runs a call → execute tool → feed result back → repeat loop until it
produces a final answer with no further tool calls. The loop lives in the
consumer - the LLM gateway stays a single request/response. The only bound is a
cap on iterations per user turn, which stops runaway loops and caps token
spend; hitting it surfaces to the user rather than failing silently.

Two kinds of errors, two destinations:

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
limited to, so the UI can use the first two camps without being misled by the
third, and vice versa for the TP-level skills.

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

Right now, skills are not built into anything - they just live in our
repository as markdown. They will need to be packaged with their requirement
tags into a registry the assistant plugin can enumerate at runtime, and
exposed to external harnesses in their native format (e.g. as installable
Claude Code skills).

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
resolved this way; skills follow the same pattern, with the caveat that for
skills the dump option remains viable even if lazy-loading is preferred.

### Self-help

From the user's point of view there is one capability here: they should be
able to ask the assistant how to use the Perfetto UI as a whole - including
the assistant itself. "How do I find the longest-running slice?", "what can
the Data Explorer do?", "how do I ask you to compare two traces?" should all
get useful answers. This is the substrate the **Teaching** UX property (see
above) is built on: the assistant can only teach the UI if it knows the UI.

The same corpus also handles **model bootstrapping** - the model reading up on
its own harness so it knows what it can do before it tries to do it. A common
failure mode of LLM harnesses is that the model has no idea what the harness
it's running inside can actually do - so it invents features, misses real
ones, and can't answer "how do I..." questions about itself. The user-facing
"how do I use Perfetto" capability and the model's own bootstrapping are the
same need served by the same docs.

The mechanism is a `read_docs(topic?)` tool (or similar) that lets the
assistant pull documentation about Intelletto and the Perfetto UI on demand -
what the UI can do, the conventions for driving it, what tools and skills
exist and how they compose. Without a topic it returns an index; with one it
returns that page. (Tool _discovery_ - how the model finds and loads callable
tools - is a separate mechanism; see Dynamic tool loading above.) Prior art:
the pi coding harness ships a read-docs tool that lets the model read its own
documentation on demand - exactly the pattern we want here.

This is deliberately a _tool_, not system-prompt bulk: the corpus can be large
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

The assistant plugin owns system prompt assembly. There is deliberately **no
free-text "extend the system prompt" hook for plugins**: the system prompt is
the most expensive real estate in the design - it is in every request of every
conversation and sits in the cache-stable prefix, with no per-use cost
pressure - so an open hook invites every plugin to dump always-on prose there.
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

If a plugin genuinely needs always-in-context knowledge not attached to any
tool or context provider, the route is a skill marked as pinned/always-loaded
(body included in the prompt rather than lazily loaded) - same effect as a
free-text hook, but it goes through one mechanism, shows up in the skills
listing for debuggability, and the prefix cost is a visible, deliberate opt-in.

Assembly rules, regardless of source:

- Contributions must be invariant for the conversation - no volatile content
  (no selection state, no timestamps-of-now). Anything per-turn belongs in a
  context provider payload instead.
- The harness concatenates contributions in a stable order (e.g. sorted by id)
  so the assembled prompt is byte-identical across turns and the cached prefix
  survives. Registration after the conversation has started takes effect on the
  next conversation (or busts the cache once, on the next turn - TBD).

Note that tool definitions themselves are not injected by us: they travel via
the API's native tool-definition channel with each request, and however the
backend folds them into its prompt is its concern - we just supply the tool
list (see Tools).

## Conversation & context management

**Conversation state.** LLM endpoints are stateless - the server keeps no
conversation, so the full message history (system prompt + every prior turn +
tool calls/results) is resent on every request. The client owns the
conversation, so we must hold it ourselves. Prompt ordering is
cache-load-bearing: backends (and local models) cache by matching prompt
prefix, so keep stable content (system prompt, skills, tool defs) at the front
and volatile content (live UI prompt-context) at the end, or we bust the cache
every turn.

**Conversation lifetime & scope.** A conversation is scoped to a single trace,
and kept in memory only. When the trace is reloaded or replaced - or the tab
is closed - the conversation is forgotten. We do **not** persist history to
local storage or anywhere else for now; the user can also reset the chat
history manually, and once it's gone it's gone.

This is an intentional Phase 1 limitation, not an oversight. We considered
letting conversations outlive the trace (registered in `onActivate`, so
context could carry across traces) - but context gathered against one trace
(selections, query results, "the stall at frame 43") is largely nonsensical
against a different trace, so carrying it over would mislead more than it
helps. Scoping to a single trace sidesteps that, and dodges the harder
questions (where history is stored, how it's keyed to a trace, stale-context
handling) until there's a reason to solve them.

The acknowledged cost is that a page reload or accidental tab close loses the
investigation. If that proves painful, revisit with per-trace persistence
keyed on a stable trace identity - but that's explicitly out of scope here.

Context compaction: initially none - it's a complex and divisive feature. Once
users reach their context window limit (as defined by an error from the
server), the assistant will simply stop working and the user will have to open
a new conversation.

## External agents via `trace_processor` as a conduit

Most of this doc is about the in-panel assistant, but the same tool surface
should also be reachable by **external coding agents** (Claude Code, Cursor,
etc.) - they can drive the UI with the same tool surface as the in-panel
assistant, though with a worse prompt posture (see below).

The cleanest path for the common Perfetto deployment is to **reuse the
existing `trace_processor --httpd` connection** as the conduit: the external
agent talks MCP to `trace_processor`, which already has a WebSocket to the UI
tab, so tool calls round-trip out to the browser and back. No new transport,
and it works in any browser (not gated on browser-extension features).

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
- **Context injection inverts from push to pull**: there is no per-turn
  injection hook into someone else's harness, so the per-turn `<ui_context>`
  block becomes a `get_ui_context()` tool returning exactly what the context
  providers would have emitted. The model has to _decide_ to call it -
  steered via the server instructions ("call `get_ui_context` before
  answering questions about 'this'") - which is strictly worse: the user
  clicks a slice and the external agent doesn't know unless it asks. The
  "point at a thing and ask" UX property largely does not survive the
  conduit.
- **`read_docs` / self-help transfers well** - it's already pull-based, so
  external agents bootstrap the same way the in-panel model does.

WebMCP / `navigator.modelContext` is the browser-native standard this is
heading toward, and we'd register tools there too since that's the direction
of travel - but today it needs a Chrome extension + a local MCP shim and only
works in Chrome Canary, so it doesn't really help us yet. The TP-conduit path
is the pragmatic one for now; treat WebMCP as something to track, not depend
on.

## Relationship to `com.google.PerfettoMcp`

There is already a plugin (`PerfettoMcp`) that does a version of this -
exposing the trace to an LLM - but in a more limited and worse way. Rather
than running two overlapping mechanisms, the plan is to fold its useful pieces
into this design and **deprecate PerfettoMcp** once the assistant's tool
surface covers what it did. The roadmap calls this out as a Phase 1 step
("merge with existing PerfettoMcp").

## Roadmap

- Phase 1 — core plumbing: Protocol/Provider/Model config layers, Gemini
  protocol, agent sidebar, basic context injection (page + selection +
  viewport), core tool surface (SQL queries, selection, timeline navigation,
  Data Explorer state), merge with existing PerfettoMcp.
- Phase 2 — richer context & extensibility: click-on-anything context
  injection, additional protocols (OpenAI-compatible, etc.), tools from other
  plugins, skills integration, external agent conduit via TP.
- Phase 3 — advanced: context compaction, more provider types, extension
  server integration, richer tool surface as plugins add their own.
