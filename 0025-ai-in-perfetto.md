# AI in Perfetto

**Authors:** @LalitMaganti

**Status:** In Progress

## Elevator Pitch

Over the past 8 years, Perfetto has become a very useful tool for understanding
the behaviour of complex software systems, especially when it comes to
performance. But the feedback we consistently hear is that people still find it
too hard to use. This is especially true for those who are new to performance
work or Perfetto, or those who do not do such work every day.

This feedback is well understood by us and reflects an intentional choice we
have made over the years to support experts with highly complex needs. These
users constantly push the boundaries of what Perfetto is capable of, and what
they want is the right data to solve really hard problems faster. This means
new data sources, better visualization tools, more powerful PerfettoSQL.

LLMs are a unique opportunity to make Perfetto more guided and interactive
without giving up the power expert users rely on. Concretely, we need to do
slightly different things for each class of users:

* **Beginner:** help them onboard onto traces, understand what they are
  looking at and do basic analysis with less domain knowledge required.
* **Intermediate:** help them access more powerful features of Perfetto like
  PerfettoSQL, Bigtrace and Data Explorer without needing to become experts
  first.
* **Advanced:** help them tie together multiple systems like Perfetto,
  Bigtrace, Data Explorer, custom scripts, benchmarking systems etc, speeding
  up workflows they already know and giving them more room to ask follow-up
  questions.

There is also the world of agents: agents recording traces, running queries,
navigating the UI and coming back with theories about what might be wrong or
trying potential fixes semi-independently of a human. While the jury is out on
exactly how reliable language models are at doing this, Perfetto should expose
the right primitives to make it as easy as possible for agents to integrate
with Perfetto tools.

Finally, "AI" doesn't just mean large language models. We also want to use more
traditional machine learning, statistical techniques and large trace corpora to
answer questions like "is the behavior in this trace normal?", "which other
traces look like this?" and "which parts of this trace are unusual?".
Basically, we should leverage the fact that we have millions of traces to draw
insights from and not restrict ourselves to only looking at the single trace in
front of us.

In this doc, I want to set out how we should change Perfetto tooling to take
advantage of the AI advances which have happened in the past year.

## Background

### Building blocks

There are lots of pieces we already have today which are critical for
leveraging AI fully.

* **[Trace Processor](https://perfetto.dev/docs/analysis/trace-processor):**
  This is the core engine for loading traces and running analysis. It will be
  at the heart of all AI workflows because it gives us a deterministic way to
  answer questions about a trace.
* **[Perfetto UI](https://perfetto.dev/docs/visualization/perfetto-ui):** This
  is the main place a lot of users interact with traces. AI should be added
  thoughtfully into the UI to help users understand what they are looking at,
  navigate to relevant parts of the trace and come up with theories on what
  might be wrong.
* **[PerfettoSQL](https://perfetto.dev/docs/analysis/perfetto-sql-getting-started):**
  This is one of the most powerful things in Perfetto but also one of the
  hardest things for many users to learn. It is the core language for asking
  precise questions about traces, and sits underneath other surfaces (e.g.
  the UI, trace processor, Bigtrace). The PerfettoSQL Standard Library also
  gives us reusable analysis building blocks, allowing agents to compose
  well-known primitives rather than write bespoke SQL for everything.
* **[Data Explorer](https://github.com/google/perfetto/discussions/5063):**
  Data Explorer is our graph-based query editor. The main reason we built it
  was to create a better representation for AI-generated trace analysis than
  raw SQL: something structured enough for machines to generate, but visual
  and concrete enough for humans to review.
* **[Server Extensions](https://perfetto.dev/docs/visualization/extension-servers):**
  We cannot and do not want to encode every possible team-specific workflow
  into Perfetto/GitHub itself. We did use to do this but we found it just
  does *not* scale and leads to a frustrating experience for everyone. Server
  extensions are the solution to this problem; they provide a modular way for
  teams to decompose customizations onto external servers.

### Sources of inspiration

There are a few existing efforts we should learn from and stay aligned with.

* **[Perfetto MCP](https://github.com/antarikshc/perfetto-mcp):** An OSS
  project making Perfetto traces analyzable through the MCP protocol. While we
  won't copy their approach for several reasons, the fact it exists and has
  attracted attention is a signal that external folks want to connect Perfetto
  to agentic tools.
* **[Chrome DevTools MCP](https://github.com/ChromeDevTools/chrome-devtools-mcp):**
  Interesting because Chrome DevTools is exposing browser/devtools state and
  control to agents. Very relevant to our thinking around a DevTools-like
  automation protocol for the Perfetto UI.
* Various other internal and external profiling tools which have attempted to
  integrate LLMs/agents.

We've also drawn on lessons from prior internal experiments at Google that
have validated demand for AI-assisted trace analysis and given us a concrete
sense of what works and what is hard when getting LLMs to reason about traces.

### Stakeholders

Interest in this direction has been expressed by a wide range of stakeholders:
multiple internal teams using Perfetto for performance work, public OSS
contributors (including @dreveman, who has proposed a [plugin-level LLM
framework](https://github.com/google/perfetto/issues/4574) and a [draft
language-model framework PR](https://github.com/google/perfetto/pull/4573)),
and external public users — including OEMs — who have raised this at Perfetto
summits and elsewhere.

### Levels of agenticness

There is a spectrum of possible AI workflows. The more independent the AI is,
the more useful it can be, but also the more likely it is to be wrong, waste
time or go down a plausible-looking rabbit hole. It's worth naming the points
on this spectrum upfront because the rest of this doc leans on them.

*Task-based workflows* are bounded requests with a clear output. For example:

* Generate a PerfettoSQL query or Data Explorer graph for a specific prompt.
* Generate a trace config to debug a specific problem (e.g. memory, jank or
  startup).

This seems very achievable and should probably be the first thing we make
really good.

*Agentic with human supervision* means asking the model to investigate while
keeping the human updated. For example:

* Find suspicious parts of a trace and show the evidence.
* Investigate likely causes of janky frames.
* Find the root cause for a startup regression.

This is where a lot of the interesting leverage is, but the framing matters.
The better version is "help me investigate and show me evidence". The worse
version is "find the root cause for me" where the tool makes a strong claim
without enough proof.

*Maximal agentic* is the far end of the spectrum: "fix this bug" by finding
the relevant code, collecting a trace, analyzing it with Perfetto, making a
fix, collecting a new trace and checking whether the issue improved. This is
an interesting idea to keep an eye on, but not something we should aim for
right now. Current models are not reliable enough for this to be a dependable
day-to-day workflow.

In practice, task-based workflows and some supervised agentic workflows are
realistic for reliable day-to-day use. More open-ended agentic investigation
should be treated carefully, and fully autonomous workflows should be treated
as highly experimental for now.

## Principles

A few principles we should adopt when integrating AI into Perfetto-related
tooling.

* **Reliability is king.** Perfetto's value rests on trust: when users see
  something in Perfetto, they believe it is telling them something real about
  the trace. LLMs are not reliable in the same way and can be confidently
  wrong, especially in agentic workflows where the model is building up its
  own theory of what happened. Concretely, this means:
  * Make it clear when something is a trace-backed fact versus a
    model-generated theory.
  * Keep the underlying evidence inspectable wherever possible.
  * Default to grounding answers in real trace data rather than model
    invention.
* **Put AI inline with existing surfaces.** Trace analysis is an iterative
  loop of looking at evidence, refining the question and following leads. AI
  is most useful when it can join that loop with the same context the user
  has, whether the user is in the UI, on the command line, or scaling a
  question through trace processor or Bigtrace. We don't force every
  investigation into a single chat surface or to a single tool.
* **Build shared, extensible primitives.** Much of the value will come from
  making Perfetto fit naturally into the AI tools people already use, like
  Gemini CLI, Claude Code, and similar agentic CLIs, not just from
  first-class AI features inside the UI. UI and non-UI workflows should share
  a common foundation: an agent working outside the UI should reason about
  traces using the same core concepts as one helping a user inside it. All of
  this should also be extensible: Perfetto should provide stable primitives
  and integration points so teams can plug in their own domain knowledge
  without us having to encode every workflow ourselves.
* **Easy way to turn off all AI features.** Especially for open source users,
  there are folks with strong moral stances against using LLMs, and we
  should respect their opinion and not force AI on them. To adhere to this,
  we should either:
  * not enable AI by default — it's probable this might naturally happen as
    you need to provide your own API key to enable anything,
  * or have a one-click prominent toggle in settings to disable all AI
    features.

## What we should build

This section sets out the high-level objectives and, under each one, the
concrete projects we should take on to deliver it. Many of the projects
**need** detailed design docs, RFCs and planning of their own so nothing here
should be treated as final. A flat scan-table appears at the end of the doc
for cross-reference.

### 1. Authoring PerfettoSQL queries in the UI

PerfettoSQL is the main way users get power out of Perfetto, but writing good
queries requires deep schema, stdlib and domain knowledge. AI can cut that
down tremendously by allowing the user to **prompt AI in natural language and
generate a PerfettoSQL query**.

The catch is that raw PerfettoSQL as it exists today is terrible to review,
especially when AI-written. We need a way for humans to actually be able to
audit what AI produces instead of having to blindly accept it because it's too
difficult to grasp.

The solution for this is twofold: **PerfettoSQL Next** and
[**Data Explorer**](https://github.com/google/perfetto/discussions/5063).
PerfettoSQL Next is an evolution of the PerfettoSQL syntax: you can think of
it as Google's pipe-syntax but heavily tuned for trace workloads, specifically
adding first-class support for intervals, trees, graphs and counters. To be
clear, this is an extension to the existing language, *not* a replacement, so
any SQL written before now keeps working just as it is. And on the other side
is Data Explorer, our graph-based query editor which we designed to have
perfect bidirectional fidelity with PerfettoSQL Next.

To be clear this means you will be able to:

* Generate a Data Explorer graph in the UI with AI.
* Save that as a PerfettoSQL query which you integrate into a SQL module in
  the standard library or elsewhere. You then build a metric on top.
* … (some months pass and out of the blue, you get a regression bug for the
  metric built on top of the query).
* Load the PerfettoSQL query in the UI and have the same graph you previously
  created appear with AI ready to help you ask questions on it.

All of this behind the scenes also requires us adding support for **AI model
selection in the UI**. Because we're open source, we need to support any model
users might want to use. This basically means allowing them to choose the
model, API key, thinking budget etc.

### 2. Interactive trace exploration in the UI

Beginner and intermediate users often don't know how to make progress in a
dense, domain-specific trace. They need the kind of guidance an expert sitting
next to them would give.

With AI, we can deliver something similar to that experience by creating an
**AI tutor for trace navigation**: a sidebar assistant that explains the
current selection, visible tracks and panel; suggests where to look next; and
creates or adjusts Data Explorer views as part of the investigation. The user
stays in control while the assistant guides exploration. The cool thing about
this is that it doesn't hide the trace behind a chat interface, it's always
visible right in front of you and you see what the agent is doing as it does
it.

To make this possible, we need the ability for the assistant to drive the UI
and understand what is being displayed right now. This means we need to
**expose UI functionality as MCP tools** which the AI can call. We already
have the inkling of the protocol with UI
[commands](https://perfetto.dev/docs/visualization/ui-automation) but we need
them to be more formalized into a type-safe and scalable protocol.

There's also the less human-supervised version of this workflow: allowing an
**AI agent to navigate the UI to independently find problems**. This builds
on top of all the functionality discussed above but with the goal of *finding
the problem* instead of *teaching the user*. The jury is out on how useful
this will actually be in practice (current feedback from users suggests
hit-or-miss behaviour) but it's clear there is real demand for this workflow.

We also shouldn't just stop on the trace-viewing side: recording the right
data is often the first hard part of debugging. **AI-generated recording
configs** let users describe the problem in natural language and have the
recording page configured for it. Again, we're not generating an opaque proto
the user blindly has to trust but instead we configure the existing recording
page to allow the user to view or tweak anything we suggest.

### 3. AI integration beyond the UI

Until now we've mostly focused on the UI, but the CLI is *just* as important,
if not more so with the way agents work today. Especially for more advanced
workflows where users might want a solution which integrates with other
systems and tools.

The trace processor is already largely in the right shape for what we need
but there's a big missing piece from the "naive" approach for querying traces
today: agents run shell in a way which causes a reparse of the trace *on
every query*. This is a total waste of time and just slows down iteration. We
solve this two-fold:

* a **long-running trace processor session + client/server**, so an agent can
  load a trace once and iterate on it across many queries. We already have an
  RPC protocol but it's quite difficult to use: we need to make it a lot
  easier.
* We have a **skill on how to use this new long-running mode** to make
  iteration a lot faster.

Next, we need a way to scale up from one trace to N traces. For up to a
hundred traces, the fastest way will still be to iterate on one machine, very
similar to how
[BatchTraceProcessor](https://perfetto.dev/docs/analysis/batch-trace-processor)
works. Then for millions, the answer is Bigtrace; it's very important at this
point you will *not* be iterating on the PerfettoSQL query but instead doing
a "one-shot" query to dump all the data into a large-scale analysis backend.
Bigtrace is *not* optimized for iterating on queries and will *always* be an
expensive way to do this. So the key ask is a **skill which explains the
"trace analysis lifecycle"**:

* iterate on one trace with long-running trace processor to come up with the
  initial PerfettoSQL queries.
* iterate on ~100 traces with a new "BatchTraceProcessor"-style API to figure
  out whether the query generalizes across traces and figure out what
  aggregation you want.
* do a one-shot query across ~million traces with Bigtrace to run on
  production workloads.

Related but orthogonal, we have the even less supervised approach of "an agent
is given a trace and is told to figure out what's wrong": I think we can add
an **opinionated "reporting" in trace processor** which will help "orient"
the agent on exactly *which* parts of the trace it should look at, which
skills it should use etc.

### 4. Allow teams to provide context to AI

There's a big part of the puzzle which we haven't discussed yet: how do we
scale this to all the teams that use Perfetto. We've learned in the past that
allowing teams to customize the Perfetto tooling is really important for
scaling and keeping the core team out of the critical path.

Of course, this applies to AI usage as well: teams
[instrument their own apps/processes](https://perfetto.dev/docs/getting-started/atrace)
with custom events, create SQL modules to process these events, create UI
macros to look into given parts of the trace, build playbooks which talk
through how to debug a regression etc.

Judging the way the winds are blowing, this is what I think we should do:

* Recommend [**Skills**](https://agentskills.io/home) as being *the* way
  teams encode information about how they want their traces to be queried,
  what events are important, what UI workflows they use etc — this is also
  what the industry as a whole is settling on.
* Create a **new "extension server" API for exposing AI skills** to Perfetto
  tools. Basically just means agreeing on an HTTP endpoint the Perfetto UI
  can query on extension servers.
* Add **integration into the UI** for reading the skills from the extension
  server.
* Have an easy **mechanism to "install" these skills** for agents for use
  with trace processor on the command line.

### 5. Trace-Shazam

Shazam lets you listen to any song and figure out "what song is this".
Suppose we did something similar to this: the user selects a region of a
current trace and can ask "have we seen something like this before?" and we
would surface similar snippets from other traces and show whether the
selected behavior is common or anomalous.

This has *nothing* to do with LLMs and is more a "machine-learning" research
problem. The Perfetto team has a bunch of different ideas on how to do this:
these range from encoding traces as some other well-understood mechanism
(e.g. sound or an image) to coming up with a unique encoding and neural
network/transformer architecture to train a model to do this.

All of this requires a lot of research. First, we **need to figure out how
to represent trace regions**, index them and retrieve useful matches;
BERT-like models are one promising direction, but audio-like or image-like
representations may also work, and have the advantage of being
better-understood ML domains. The other problem is **trace segmentation +
region selection**: deciding what unit to compare in the first place —
automated trace segmentation that figures out which parts of the trace even
matter.

A separate hard problem is obtaining good training data: in particular,
ground-truth labels of "what matters in a trace" and "what experts pay
attention to" are not something we have today, and we'll need to figure out
how to get them in a way that is consistent with Perfetto's privacy posture.

### 6. Unsupervised learning to find what makes traces different

Trace-Shazam answers "are these similar?". The harder question is "what's
different and why?". A similarity system can say "these regions look alike"
without knowing why that matters. We want something that can say "this stall
is longer than normal", "this pattern usually appears with this other event",
or "experts tend to inspect this part next".

Again this is a non-LLM problem at its core but LLMs *can* play a role here
to perform the explanation from the core grounded results. It's also *much*
harder and really needs us to make a lot of progress on Trace-Shazam and have
a working implementation before we can do anything too useful here.

## Project index

| Project                                                | Surface  | Supports          |
| :----------------------------------------------------- | :------- | :---------------- |
| PerfettoSQL Next                                       | Language | (1), (3)          |
| AI model plumbing for the UI                           | UI       | (1), (2), (3)     |
| AI prompt → Data Explorer graph                        | UI       | (1)               |
| AI-generated recording configs                         | UI       | (2)               |
| AI tutor for trace navigation                          | UI       | (2), (3)          |
| Real API/RPC layer for the UI                          | UI       | (2), (3)          |
| Investigate "agentic flow" in the UI                   | UI       | (3)               |
| Long-running trace processor session + client/server   | CLI      | (3)               |
| Skills for PerfettoSQL generation and querying         | CLI      | (1), (3)          |
| Skills for trace config generation / recording         | CLI      | (3)               |
| Skills for trace summarization                         | CLI      | (2), (3)          |
| Trace processor support for installing skills locally  | CLI      | (3)               |
| Trace processor support for connecting to ext. servers | CLI      | (3)               |
| Skill for large-corpus SQL + Bigtrace                  | CLI      | (3), (4)          |
| Skills extension server API                            | Shared   | (1), (2), (3)     |
| Skills templates / examples                            | Shared   | (3)               |
| Agent access security model                            | Shared   | (3)               |
| Similarity search design                               | Research | (5)               |
| Trace segmentation / region selection                  | Research | (5), (6)          |
| Models for what is normal vs different                 | Research | (6)               |
