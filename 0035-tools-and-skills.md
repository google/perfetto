# Perfetto UI: Assistant tools and skills

**Authors:** @stevegolton

**Status:** Draft

## Introduction

This doc specifies the assistant's **tools** and **skills** - the extensible
capability surface that plugins and the core register against. Tools are how the
model acts on the trace and UI (run queries, drive the timeline, build graphs);
skills are reusable prompt-text playbooks that teach the model how to use the UI
and diagnose traces.

This is one of the docs split out from
[RFC-0029](0029-intelletto.md), the top-level coordination doc for the AI
assistant work. The assistant UX, agent loop, and system-prompt assembly live in
[RFC-0032](0032-embedded-assistant.md); the provider-agnostic plumbing in
[RFC-0033](0033-llm-framework.md); context injection in
[RFC-0034](0034-context-injection.md); the external-harness conduit (which
consumes this same surface) in [RFC-0036](0036-external-agent-mcp.md). The wider
motivation is in
[RFC-0025](https://github.com/google/perfetto/discussions/5763).

## Tools

Tools are how the model interacts with the trace and the UI: run queries, build
Data Explorer graphs, select events, switch pages, add debug tracks, etc. Tools
are provided by plugins (a tool is ultimately a callback) and registered with
the core registry, the same way as context providers (see
[RFC-0034](0034-context-injection.md)).

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

Either way, the registry itself lives in core alongside the gateway (see
[RFC-0033](0033-llm-framework.md)): the assistant is just one consumer of it
(external harnesses are another), and registering a tool shouldn't require a
dependency on the assistant plugin.

### Registering a tool

This is option 1's shape (and what the model-only tools need under either
option). A command is just a name + callback; a tool needs more, because the
model has to _decide_ to call it and call it _correctly_, and the harness has
to validate, execute, and serialise the result. For example:

```ts
trace.llm.registerTool({
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
  // (down-converted per backend by the Provider layer, see RFC-0033), and
  // runtime validation of the model's args. Field descriptions matter - the
  // model reads those too.
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
- The allowlist is a core registry, not a static file: core
  ships its entries, and third-party plugins contribute their own command
  ids through the same API at runtime.

(In the hybrid landing point from the open question above, model-only
capabilities would instead use the standalone `registerTool` API and never
enter the command registry - the palette allowlist is then only needed for
edge cases.) Commands gaining schemas is also the first step the UI RPC
protocol (see [RFC-0036](0036-external-agent-mcp.md)) needs.

**Aside - commands as an instrumentation surface (speculative).** If we push the
convention further so that even plain buttons invoke a (schema'd) command rather
than calling a handler inline, every meaningful UI action flows through one
registry - which is then a natural choke point to _log_ what users do: which
command, with which typed args, in what order. That interaction stream is
valuable beyond the immediate action: mining the common workflows into skills,
building eval sets for the assistant, and grounding "show, don't tell"
demonstrations (see [RFC-0032](0032-embedded-assistant.md)) in what users
actually do rather than guesses. Strictly a possibility the command-schema work
opens up, not a committed feature - and any such logging is subject to the same
opt-in / privacy posture as everything else (see
[RFC-0029](0029-intelletto.md)); nothing is collected from the hosted UI without
consent.

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
as markdown. They will need to be packaged with their requirement tags into
the core skill registry the assistant can enumerate at runtime, and exposed to
external harnesses in their native format (e.g. as installable Claude Code
skills; see [RFC-0036](0036-external-agent-mcp.md)).

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
UX property (see [RFC-0032](0032-embedded-assistant.md)) is built on: the assistant can
only teach the UI if it knows the UI.

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
out of the cache-stable prefix (see prompt ordering in
[RFC-0032](0032-embedded-assistant.md)) and out of the context until the assistant
actually needs it.

The docs should be authored as content (not generated solely from tool
schemas) so they can carry the _why_ and the worked examples - how to chain
`get_schema` -> `run_query` -> `create_de_graph`, when to aggregate vs. pull
rows, etc. - the things a bare schema can't convey. For the source of truth,
the same skill/tool registries that back the discovery tools should back this,
so the docs can't drift from what's actually registered. A tool's description
and a skill's body are the docs; `read_docs` just surfaces them plus any
hand-written overview pages.

## Open questions

1. **Tools vs. commands**: whether tools are their own registration mechanism,
   or commands grow optional descriptions + input schemas with an allowlist
   deciding model exposure - or where the hybrid line sits between the two.
   (Either way the registry lives in core, alongside the gateway in
   [RFC-0033](0033-llm-framework.md).)
