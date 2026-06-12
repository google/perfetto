# Perfetto UI: Embedded Assistant

**Authors:** @stevegolton

**Status:** Draft

## Introduction

This doc specifies the **embedded assistant** - the in-UI sidebar chat that lets
users analyze their traces and operate the Perfetto UI using natural language.

For example, the assistant should be able to help with:

- **Answering questions about their trace**: click a janky frame and ask "why
  did this frame miss its deadline?", or "what was the main thread doing during
  this 200ms gap?"
- **Writing queries**: "show me the 10 longest binder transactions in this
  selection" becomes SQL without the user knowing the table layout.
- **Building node graphs in Data Explorer**: "break this down by process, then
  bucket by 10ms intervals." or "modify this node graph to show slice self time
  rather than total time".
- **Navigating the trace and selecting events**: "take me to the first GC pause
  after app startup."
- **Teaching users how to use the UI**: "how do I pin this track?" or "what does
  this counter track actually measure?"

The real value-add of embedding the assistant in the UI - as opposed to a 3rd
party harness connected via a bridge - is twofold:

1. The possibility of integrating the UX more closely with the UI. For example:
**click-to-context**: the user points at elements in the UI and that context
travels with the prompt. The full argument lives in the UX Design section below.
1. For users who want to use these tools without having to install anything
extra. The UI has traditionally attempted to make all features available in the
browser by default for convinence (e.g. the embedded WASM trace processor) with
the option to customize if desired (e.g. websocket connected trace processor
instance running locally).

It is one of the docs split out from [RFC-0029](0029-intelletto.md), the
top-level coordination doc for the AI assistant work, which owns the overall
motivation, the component breakdown, the roadmap, and the cross-cutting risks.
This doc covers **component 1** itself: its UX, the agent loop, the system
prompt, and conversation management. The pieces it builds on live in sibling
docs - the provider-agnostic plumbing in [RFC-0033](0033-llm-framework.md),
context injection in [RFC-0034](0034-context-injection.md), tools and skills in
[RFC-0035](0035-tools-and-skills.md), and the external-harness conduit in
[RFC-0036](0036-external-agent-mcp.md). The wider motivation - who AI tooling in
Perfetto serves and why - lives in
[RFC-0025](https://github.com/google/perfetto/discussions/5763).

## UX

### UX Requirements

- **Natural Language Prompting**: The user can interact with the assistant using
  natural language.
- **Conversational (multi-turn context accumulation)**: Investigations are
  rarely solved one-shot. Users should be able to ask follow-up questions,
  leaning on the combined context from any previous questions and responses.
- **Context travels with the user**: Context should not be tied to a given page,
  panel or element. The user must be able to navigate the UI, bringing any
  existing chat context with them, asking followup questions within a different
  context.
- **Rich context injection**: The model should be delivered relevant information
  based on what the user is currently looking at. It should be able to infer the
  meaning of the word 'this' from the context provided.
- **Agency**: The assistant acts, it doesn't just answer. It acts for two
  distinct reasons:
  - **To gather what it needs** - rather than waiting for the user to bring the
    relevant information into context, the assistant finds it itself (e.g.
    running SQL to inspect the trace).
  - **To act on the user's behalf in the UI** - carrying out the user's intent
    by driving the UI (completing SQL queries, building Data Explorer graphs,
    moving the selection, navigating between pages) rather than only describing
    what the user should do. In both cases the user describes intent; the
    assistant carries it out, rather than only responding with text.
- **Teaching, not just doing**: A complement to Agency - sometimes the most
  useful thing the assistant can do is show the user _how_ to do something
  themselves, rather than silently doing it for them. Perfetto's UI is deep and
  much of its power is undiscovered; the assistant is well placed to surface the
  relevant feature, query, or workflow and explain it. Where it does act, it
  should prefer to **show, not tell**: carry the task out using the same UI
  affordance the user could have used - e.g. drive the search box to locate a
  slice rather than silently resolving it through SQL - and name the feature
  (and where it lives) so the user can repeat it next time without the
  assistant. A one-off answer becomes a transferable skill. The knowledge that
  backs this - what the UI can do and how to drive it - lives in self-help and
  skills (see [RFC-0035](0035-tools-and-skills.md)).

### UX Design

The assistant's UX surface is centered around three main concepts:

- **Chat in a sidebar**: This is a very common UX found in lots of AI tools, and
  for a good reason: it presents the chat history in one place and provides a
  persistent, always-available prompt box which can be used to ask followup
  questions given the current context. Being in a sidebar means it follows the
  user around the UI even through page flips. It should be an obvious UX for
  anyone who's used an AI powered tool before.
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

RFC-0025 sets two quality principles this design must implement: make it clear
when something is a **trace-backed fact versus a model-generated theory**, and
keep the **underlying evidence inspectable**. They cut in both directions:

- **Inputs**: the user must be able to see exactly what the model sees - the
  context strip in the chat UI, with its expand-to-raw-payload view, provides
  this (see [RFC-0034](0034-context-injection.md)).
- **Outputs**: the transcript renders the model's work, not just its conclusions
  - every tool call and its result (the SQL that was run, the rows that came
  back) is visible inline, so any numeric claim can be traced to the query that
  produced it. A claim with no tool call behind it is visibly a theory. Tools
  like `add_debug_track` push this further: findings land on the timeline next
  to the evidence, where the user can verify them against the trace directly.

Finally, per RFC-0025's user-control default: the assistant is opt-in - it stays
out of the way (and sends nothing anywhere) until the user has explicitly
configured or accepted a provider, and we will have a prominent setting that
turns all AI assistant features off.

## Agent loops

The model runs a call → execute tool → feed result back → repeat loop until it
produces a final answer with no further tool calls. The loop lives in the
consumer: it opens a `Conversation` on the chosen model (see
[RFC-0033](0033-llm-framework.md)) and drives it with `sendTurn` - the user's
prompt first, then each tool result - the provider streaming a reply each time.
The only bound is a cap on iterations per user turn, which stops runaway loops
and caps token spend; hitting it surfaces to the user rather than failing
silently.

Errors are routed to one of two destinations:

- **Tool errors go to the model** as the tool result (e.g. a SQL syntax error)
  so it can self-correct and retry, bounded by the iteration cap.
- **Backend and loop errors go to the user**: rate limit, auth failure, model
  unreachable, context-length exceeded, iteration cap hit. These render inline
  in the chat window - the chat is the single transcript of everything that
  happened, successes and failures alike, so the user always sees why a turn
  stopped. The Provider layer already normalises backend errors into a common
  shape (see [RFC-0033](0033-llm-framework.md)); the chat just renders it.

Cancellation & steering - because the loop can fire many tool calls, the user
needs a way to intervene:

- **Cancel** (Phase 1): abort the in-flight request (`AbortController`) and stop
  the loop from starting the next iteration. Keep what completed, marked
  interrupted, so the transcript stays truthful.
- **Queued follow-up** (Phase 1, cheap): a message typed mid-loop is enqueued
  and picked up at the next turn boundary - one check at the top of the loop, no
  interruption machinery. Covers most of what users want ("oh wait, also check
  X").
- **True mid-flight steering** (deferred): injecting the message _into_ the
  running loop needs safe interruption points and in-flight tool-call handling
  - real machinery, not worth it while cancel + queued follow-up cover the
    common cases.

Cost / token visibility: the Provider layer already reports token usage per
request (see [RFC-0033](0033-llm-framework.md)), so we can surface running usage
to the user - tokens (and, where the provider gives a price, cost) per turn and
per conversation. Matters for anyone on a per-token plan or a quota, especially
since the agent can loop and spend more than a single round-trip would suggest.
Probably a lightweight indicator rather than Phase 1 core; noting it so the
usage the Provider layer surfaces doesn't go unused.

## System prompt

The assistant plugin owns system prompt assembly. There is **no free-text
"extend the system prompt" hook for plugins**: the system prompt is sent with
every request of every conversation and sits in the cache-stable prefix, so an
open hook invites every plugin to dump always-on prose there. Instead,
prompt-worthy content is colocated with the artifact that needs it, and the
assembled prompt concatenates:

- **Application brief** - the fixed prompt of whichever consumer is driving the
  gateway; the conversational assistant's ("you are in a trace viewer tool
  called Perfetto and your job is to help diagnose issues by looking at a
  trace") differs from e.g. a trace summariser's.
- **Model-specific prompt** - from the provider/model config (see
  [RFC-0033](0033-llm-framework.md)), for per-model fine tuning (e.g. "Don't
  mention goblins").
- **Context provider `description`s** - payload format explanations, included
  while the provider is registered (see
  [RFC-0034](0034-context-injection.md)).
- **Tool descriptions** - when-to-call-me guidance, carried by the tool
  definitions themselves.
- **Skill index** - names + descriptions; full bodies load lazily (see
  [RFC-0035](0035-tools-and-skills.md)).
- **User instructions** - a settings textbox for end users ("my app's main
  thread is called WorkerPool-3", "show me queries before running them") -
  appended last so the harness's own invariants stay authoritative.

Assembly rules, regardless of source:

- Contributions must be invariant for the conversation - no volatile content (no
  selection state, no timestamps-of-now). Anything per-turn belongs in a context
  provider payload instead.
- The harness concatenates contributions in a stable order (e.g. sorted by id)
  so the assembled prompt is byte-identical across turns and the cached prefix
  survives. Registration after a conversation has started takes effect on the
  next conversation, to avoid busting the cache.

Note that tool definitions themselves are not injected by us manually in to the
user turn: they are submitted via the API's native tool-definition channel with
each request, and however the backend folds them into its prompt is its concern,
we just supply the tool list (see [RFC-0035](0035-tools-and-skills.md)).

## Conversation & context management

**Conversation state.** We hold the canonical transcript (system prompt + every
prior turn + tool calls/results) ourselves regardless - it drives rendering,
export and reset. How it reaches the backend is the provider's concern, behind
the `Conversation` handle (see [RFC-0033](0033-llm-framework.md)): most backends
are stateless and the handle resends the full history each turn, but a stateful
one (e.g. Chrome's Prompt API) may keep its own session and take only the delta.
Either way, prompt ordering matters for caching: stateless backends (and local
models) cache by matching prompt prefix, so keep stable content (system prompt,
skills, tool defs) at the front and volatile content (live UI prompt-context) at
the end, or we bust the cache every turn.

**Conversation lifetime & scope.** A conversation is scoped to a single trace,
and kept in memory only. When the trace is reloaded or replaced - or the tab is
closed - the conversation is forgotten. We do **not** persist history to local
storage or anywhere else for now; the user can also reset the chat history
manually, and once it's gone it's gone.

We considered letting conversations outlive the trace (registered in
`onActivate`, so context could carry across traces) - but context gathered
against one trace (selections, query results, "the stall at frame 43") is
largely nonsensical against a different trace, so carrying it over would mislead
more than it helps. Scoping to a single trace sidesteps that, and dodges the
harder questions (where history is stored, how it's keyed to a trace,
stale-context handling) until there's a reason to solve them.

The cost is that a page reload or accidental tab close loses the investigation.
If that proves painful, revisit with per-trace persistence keyed on a stable
trace identity - but that's explicitly out of scope here.

Context compaction: initially none - it's a complex and destructive feature.
Once users reach their context window limit (as defined by an error from the
server), the assistant will simply stop working and the user will have to open a
new conversation.

We will provide an export button to allow users to export their conversation to
another tool if they so desire.
