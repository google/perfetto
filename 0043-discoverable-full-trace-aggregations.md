# Discoverable Full-Trace Aggregations

**Authors:** @LalitMaganti

**Status:** Discussion

**PR:** N/A

This RFC is for discussion only. There are currently no plans to implement the
strawman described below.

## Problem

Perfetto can aggregate many kinds of trace data, but these aggregations are
usually discoverable only after the user creates an area selection over the
right timeline tracks. This introduces unnecessary friction for common
questions such as:

* What are the slowest slices in this trace?
* Which slice categories account for the most time?
* Which processes consumed the most CPU time?
* What does the flamegraph of all stack samples look like?

Answering these questions should be a natural starting point when opening a
trace. This is the workflow offered by profilers such as Instruments: useful
summaries of the captured data are immediately available, and the user can
start from them before narrowing their investigation.

In Perfetto today, the user instead needs to:

1. Know that an aggregation exists.
2. Find the timeline tracks which expose the relevant data.
3. Create an area selection covering those tracks and the desired time range.
4. Find the resulting aggregation in the selection details panel.

This workflow is valuable when the user wants to aggregate a particular time
range or set of tracks. It is unnecessary ceremony when the user simply wants
a quick aggregate over the whole trace.

More fundamentally, the available aggregations are indirectly advertised by
the timeline. The existing aggregation adapter receives an `AreaSelection`, and
aggregators generally inspect the selected `Track` objects to discover their
input data. For example, slice aggregation collects datasets from selected
track renderers, while counter aggregation obtains counter track IDs from the
selected tracks' tags.

This couples discoverability to presentation:

* An aggregation is not visible until the right tracks are selected.
* The user must understand the timeline representation before discovering the
  aggregate.
* Data without a timeline representation has no equivalent route to advertise
  an aggregate.
* Whether an aggregation can be found depends on which data was chosen for
  display on the timeline.

The goal of this RFC is narrowly about **discoverability and ease of use**.
When a loaded trace contains data for which a plugin can provide a useful
aggregate, the user should be able to discover and open that aggregate without
first making a selection or finding its timeline representation.

## Goals

* Make useful whole-trace aggregations discoverable from the bottom panel as
  soon as a trace is loaded.
* Allow plugins to expose aggregates for data which does not appear on the
  timeline.
* Make common aggregate-first workflows fast, including flamegraphs of stack
  samples, the most expensive slices, and processes ranked by CPU consumption.
* Preserve the ability of the caller to decide what data to aggregate and what
  to show.
* Reuse existing aggregation table and visualization machinery where that is
  practical.

## Non-goals

* Replacing or changing area-selection aggregation. Area selections remain the
  appropriate way to aggregate a particular time range and set of tracks.
* Automatically turning every area-selection aggregation into a full-trace
  aggregation.
* Creating a general-purpose "analysis" framework. "Analysis" is broad and
  overloaded; this RFC concerns discoverable full-trace aggregates.
* Requiring every bottom-panel view to use one abstraction. Existing views such
  as Android Logs and Ftrace Events may share presentation or registration
  machinery where useful without being forced into an aggregation model.
* Prescribing one visualization. A caller might expose a table, flamegraph, or
  another purpose-built aggregate view.
* Committing to the API sketched below. The design is a strawman intended to
  make the API and lifecycle discussion concrete.

## Decision

Pending.

## Existing architecture

The public extension point for selection-dependent content is
`SelectionManager.registerAreaSelectionTab()`:

```ts
interface AreaSelectionTab {
  readonly id: string;
  readonly name: string;
  readonly priority?: number;

  render(selection: AreaSelection): ContentWithLoadingFlag | undefined;
}
```

An `AreaSelection` contains both a time range and resolved timeline tracks:

```ts
interface AreaSelection extends Area {
  readonly kind: 'area';
  readonly tracks: ReadonlyArray<Track>;
}

interface Area {
  readonly start: time;
  readonly end: time;
  readonly trackUris: ReadonlyArray<string>;
}
```

The aggregation adapter builds an `AreaSelectionTab` around an `Aggregator`:

```ts
interface Aggregator {
  readonly id: string;
  probe(area: AreaSelection): Aggregation | undefined;
  getTabName(): string;
  getGridConfig(): AggregatorGridConfig;
}
```

`probe()` serves two related purposes. It determines whether the aggregator
applies to the current selection, and it captures the inputs required to
prepare the aggregate. Returning `undefined` hides the tab.

This arrangement works well for selection-dependent aggregation because the
selection supplies:

* The selected time range.
* The selected tracks.
* Datasets exposed by those tracks' renderers.
* Track tags, including Trace Processor track IDs.
* Lineage back to timeline tracks for interactions with aggregate results.

It does not naturally answer which aggregates should be offered before a
selection exists. Substituting the trace bounds for `start` and `end` is
insufficient: there are no selected tracks from which to discover datasets,
track IDs, or applicability.

Perfetto also has a general bottom-panel tab API. Plugins such as Android Logs
and Ftrace already register trace-wide tabs and expose commands to open them.
This provides useful precedent for presentation, but it does not provide a
catalogue of aggregates which the bottom panel can automatically surface.

## Strawman design

This section describes one possible design to anchor discussion. It is
intentionally a strawman: the important proposal is the product behavior and
the separation from timeline discovery, not the precise TypeScript API below.

### Full-trace aggregation registration

Add a public, trace-scoped registry for full-trace aggregates. A plugin
registers an aggregate after determining that its input data is present in the
loaded trace:

```ts
trace.fullTraceAggregations.register({
  id: 'dev.perfetto.StackSamples',
  title: 'Stack samples',
  render: () => m(StackSampleFlamegraph, {trace}),
});
```

Registration advertises availability. Registered entries are automatically
listed in a persistent, discoverable part of the bottom panel; the user does
not need to run a command, find a timeline track, or create a selection first.
The panel can remain lazily rendered so that registration does not imply eager
computation of every aggregate.

The caller owns the semantics and presentation of its aggregate. For example:

* A stack-profile plugin can register a whole-trace flamegraph.
* A slice plugin can register a table initially grouped and sorted by total
  duration.
* A scheduling plugin can register processes ranked by CPU time.
* A domain-specific plugin can register an aggregate over data which has no
  timeline track at all.

The API should support a standard tabular path which reuses the existing
`AggregationPanel`, `SQLDataSource`, grid configuration, export support, and
loading states. It should not make a SQL table mandatory for aggregates such as
flamegraphs.

A more structured variant of the strawman could therefore distinguish standard
aggregation tables from custom content:

```ts
trace.fullTraceAggregations.register({
  id: 'dev.perfetto.Slices',
  title: 'Slices',
  content: {
    kind: 'table',
    prepareData: async (engine) => ({tableName: 'slice_aggregation'}),
    gridConfig: sliceGridConfig,
  },
});

trace.fullTraceAggregations.register({
  id: 'dev.perfetto.StackSamples',
  title: 'Stack samples',
  content: {
    kind: 'custom',
    render: () => m(StackSampleFlamegraph, {trace}),
  },
});
```

The exact split between a standard path and custom rendering is left open.

### Availability is independent of timeline tracks

The defining property of the registry is that availability is declared by the
plugin, not inferred from the current workspace or selection.

In the simplest lifecycle, plugins conditionally register during
`onTraceLoad()`:

```ts
async onTraceLoad(trace: Trace): Promise<void> {
  if (await hasStackSamples(trace.engine)) {
    trace.fullTraceAggregations.register(...);
  }
}
```

This follows the existing plugin lifecycle: `onTraceLoad()` can inspect Trace
Processor and register trace-specific UI. It also keeps the registry simple;
the framework does not need a second probing lifecycle.

An alternative is to always register a descriptor with an asynchronous
`probe()` method and let the framework decide whether to show it. That could
centralize loading and error handling but introduces more lifecycle machinery.

Either version satisfies the central requirement: a plugin can advertise an
aggregate based on data in the trace even when that data is not represented by
a timeline track.

### Relationship to area-selection aggregation

The new registry is additive. It does not change
`registerAreaSelectionTab()`, `AreaSelection`, or the behavior of current
selection aggregators.

Implementations may share lower-level query and rendering code. For example, a
slice aggregation could factor out code which accepts a set of input datasets
and a time range, then invoke it from both:

* An area-selection adapter using selected tracks and the selected interval.
* A full-trace registration using independently discovered inputs and the trace
  bounds.

However, existing area-selection aggregators cannot be reused automatically.
Their `probe()` methods commonly derive their inputs from `area.tracks`, and
some aggregates are meaningful only for an explicit subset of tracks. Each
plugin should deliberately decide whether it has a useful full-trace aggregate
and how to source its inputs.

### Finding input data

There are two broad cases.

#### Canonical Trace Processor data

Some plugins can detect and aggregate their data directly through Trace
Processor tables or modules. Android logs, Ftrace events, scheduling data, and
canonical stack samples are examples. These do not need timeline tracks for
availability or input discovery.

#### Plugin-defined datasets

The generic slice area-selection aggregator supports datasets exposed by track
renderers, including custom slice-like datasets. There is currently no
trace-level registry containing all such datasets independently of tracks.

The strawman does not attempt to create one. Initially, the plugin registering
the full-trace aggregate is responsible for retaining or reconstructing the
inputs it needs. If multiple use cases emerge which require discovering
plugin-defined datasets independently of timeline tracks, a trace-level dataset
registry can be considered separately.

This constraint should not weaken the product requirement: having no timeline
track must not prevent a plugin from directly registering useful aggregate
content.

### Bottom-panel presentation

Registered full-trace aggregates should be visible as choices in the bottom
panel without an active selection. The precise presentation—tabs, a chooser, or
another compact affordance—is left open, but it should satisfy the following:

* A user opening a trace can discover which full-trace aggregates are available.
* Opening an aggregate does not create or mutate a timeline selection.
* Creating a later selection continues to expose the existing selection tabs.
* Expensive aggregates may compute lazily when opened.
* The bottom panel can continue to host existing non-aggregate content.

The persistent surface may also provide a scope selector in the future, for
example between the full trace and the currently visible window. This is not
required by the initial proposal. Starting with a fixed full-trace scope keeps
the behavior clear and addresses the primary discoverability problem.

## Examples

### Stack-sample flamegraph

A trace contains stack samples but the user has not added or selected a profile
track. The responsible plugin detects the samples at trace load and registers
"Stack samples". The entry is immediately discoverable in the bottom panel and
opens a flamegraph over all samples in the trace.

The user may later select a time range to obtain the existing selection-scoped
flamegraph. Neither surface replaces the other.

### Most expensive slices

A plugin registers "Slices" with a table grouped by slice name and initially
sorted by total duration. This gives the user an immediate answer to which slice
categories consumed the most time. The table may additionally allow the user
to change grouping, filtering, and sorting using the existing data grid.

### CPU consumption by process

A scheduling plugin registers "CPU by process" after detecting scheduling data.
It aggregates CPU running time over the trace and ranks processes by total
consumption. This is available even if the relevant scheduling tracks are not
visible in the current workspace.

## Alternatives considered

### Require users to select the whole trace

Perfetto could make it easier to create an area selection spanning every track
and the complete trace bounds.

This reduces the number of gestures but does not solve discoverability. The
user must still know that an aggregation exists and that creating a selection
will reveal it. It also preserves the coupling between available aggregates and
timeline tracks, excluding data with no timeline representation.

### Synthesize an `AreaSelection`

The bottom panel could construct an `AreaSelection` using the trace bounds and
all registered tracks, then pass it to existing aggregators.

This offers some implementation reuse but gives selection-specific APIs
misleading semantics. It would aggregate only datasets advertised by tracks,
its results would depend on the set of registered or visible tracks, and it
would still exclude non-timeline data. It could also expose aggregators which
are inappropriate outside an intentional track selection.

### Generalize `AreaSelection` into an aggregation context

The existing APIs could be redesigned around a generic context containing a
time span and optional input tracks.

This may be a useful refactoring after common requirements are understood, but
it does not itself create a source of discoverability or trace-level input data.
It also risks broad churn to a working area-selection system. The strawman keeps
the new surface additive and permits sharing implementation below the public
registration layer.

### Use ordinary bottom-panel tabs and commands

Plugins can already register arbitrary tabs and commands. Android Logs and
Ftrace Events use this pattern.

This is sufficient for individual bespoke features, but there is no common,
automatically discoverable catalogue of full-trace aggregates. Each plugin must
invent how users find and open its content. A dedicated registration point gives
the bottom panel enough information to surface all applicable aggregates
consistently while still allowing custom rendering.

### Introduce a trace-level dataset registry first

A generic registry could allow plugins to publish aggregateable datasets
independently of both tracks and aggregates. Full-trace aggregators could then
discover compatible datasets by schema.

This would provide a stronger separation between data and presentation, but it
is substantially broader than the user problem in this RFC. It also requires
answers about ownership, identity, lineage, deduplication, and lifecycle. Direct
aggregate registration is a smaller way to validate the desired workflow. A
dataset registry can follow if repeated use cases justify it.

## Open questions

* What is the smallest useful public registration API while supporting both
  standard table aggregates and views such as flamegraphs?
* Should plugins conditionally register from `onTraceLoad()`, or should the
  framework own an asynchronous availability probe?
* How should registered aggregates be presented in the bottom panel so they are
  discoverable without consuming excessive space?
* Which existing full-trace views, such as Android Logs and Ftrace Events,
  should participate in the same discoverability surface without being modeled
  as aggregates?
* Which queries and rendering components should be factored out of existing
  area-selection aggregators for reuse by full-trace registrations?
* Should a visible-window scope be added later, or is full-trace plus explicit
  area selection sufficient?
