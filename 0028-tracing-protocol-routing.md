# Tracing Protocol Redesign: Routing, deduping and demuxing

**Authors:** @primiano

**Status:** Draft

**PR:** N/A

This is a follow up to [RFC-0014][rfc14] and focuses
on the topic of routing and deduping track events and trace packets across
multiple sessions.

[rfc14]: 0014-tracing-protocol-redesign.md

## Problem statement

In the current design of Perfetto, a data source can be instantiated N times
(e.g. if creating N tracing sessions each with the same data source). Today
that is fully supported, but comes at the cost of putting more load on the
writer, who needs to write the data N times. This semed a good tradeoff in 2017
when we started the project.

As things evolved, however, we came to the realization that multiple concurrent
tracing session are an everyday reality, and the trend will only grow over time.
This is mainly due to Perfetto having a large number of customers, each of them
needing a different variant of a trace config.

Today we have two options to deal with it, and none of them is ideal:

1. Mix all the needs of the various customers into one big "AOT" trace config.
   This scales performance-wise (there is only one instance of ftrace, only one
   track-event per process) but doesn't scale trace-banwidth-wise, as the one
   trace config becomes a superset of everybody's needs. The trace duration gets
   shorter and shorter over time, as the bandwith increases.

2. Break down into distinct configs. This is possible only to a certain extent,
   but creates big problems in terms of SMB pressure and CPU overhead on the
   writing paths.

What we would like instead is a mechanism such that we could have N independent
tracing sessions, each with their own configs, e.g.:

```
session 1:
  - data source 1: ftrace
    - ftrace_event: sched_switch
    - ftrace_event: sched_waking
   - data source 2: track_event
     category: net
     category: art

session 2:
  - data source 1: ftrace
    - ftrace_event: sched_switch
    - ftrace_event: task_newtask
   - data source 2: track_event
     category: net
     category: webview
```

but make it so that the "sched_switch" ftrace events and the "net" track events
are emitted only once.

Intuitively we need to create some notion of "routing ID" which is decoupled
from today's strict notion of "destination buffer ID", to then allow traced to
handle copies on the receiving side.

This will still require a memcpy, but the cost is moved to traced, which has two
significant benefits:
- Reduced load and complexity from the writer side, which is the fastpath.
- Traced operates in batches, leading to better performance characteristics
  (caches are hot, scaling governors ramped up)
- Generally the CPU cost of traced is less of a concern because it's a
  background, non-user-facing service.

### The pursuit of a generalized mechanism

It is certainly possible to devise a solution, rather earily, that is very
specific to track event or ftrace. However that comes with a big architectural
downside: it makes implementation details of other data source bleed into
traced, which is unprecedented.

I am looking for a solution that makes the aforementioned use cases possible
without adding technical debt to traced.

Also a generalized solution is desirable as there are other classes of problems
(e.g. GPU counters, which use the data source layer of the SDK) that could
benefit from this.

## Proposal overview

The idea is to prepend each message in the SMB ring buffer with a `Routing ID`
(here, in short, `RID`).

The definition of how a RID is derived is owned by the data source and hence
implementation-dependent and is **completely opaque to traced**. This opacity is
the single most important property of the design: it is what keeps the
implementation details of each data source from bleeding into traced.

- The ftrace data source will create one RID for each trace point enabled.
- The track event data source will create one RID for each category.

Traced doesn't care about what the RID is. The only thing it cares about is the
notion of "RID 1 must go into buffers 2 and 4, RID 2 into buffer 3,4 and 5"
which is pushed by the data source. Traced is, in other words, a pure
integer-keyed router.

The rest of this document is organized as follows:

- **Control plane**: how RIDs are assigned and how the `RID -> buffers` map is
  negotiated between producers and traced.
- **Data plane**: how RIDs are carried in the SMB ring buffer.
- **Demultiplexing and sequence state**: the consequences of emitting a sequence
  once and fanning it out to many buffers, and how per-sequence incremental
  state is handled. This is the most subtle part of the design.
- **Edge cases**: config divergence and dynamically-registered RIDs.
- **Relationship with QoS channels**: a short note; the details belong to a
  separate RFC.

## Control plane

### StartDataSource (traced -> producer)

There is no protocol change here. Traced tells the producer
_"create a new instance of the ftrace data source, and enable the trace points
[sched_switch, sched_waking, ...]"_

At this point the data source goes through the list of trace points (or
categories, in the case of track event) and associates one RID for each of them.

Note that the matching of the config (e.g. globs like `enabled_categories: "*"`,
or the expansion of atrace categories into ftrace events) is done entirely
**producer-side**, as it is today. Traced never sees category names or globs,
only the resulting integer RIDs. This is what keeps the matching logic (which is
inherently data-source-specific) out of traced.

### How is a RID assigned? How do we prevent conflicts across processes?

I see three options here:

- **Option 1**: RIDs are globally-scoped 32-bit hashes.
  The ftrace data source computes the RID as
  `HASH(concat("linux.ftrace", trace_point_name))`; the Track Event data
  source computes them as `HASH(concat("track_event", category_name))`.
  I'm making the bullish (but realistic) assumption that the chances of FNV-1a
  (or MurMur) collisions within thousands of categories/trace points is minimal.
  We make the same assumptions elsewhere (e.g. TP strings) and that has served
  us well. The advantage of this option is that assignment is completely
  stateless and lock-free: the same stream always hashes to the same RID, so
  dedup across sessions is automatic and requires no registry.

- **Option 2**: RIDs are a monotonic counter, scoped per-producer. This means
  that each producer needs to maintain a registry for its own RIDs, and traced
  needs to do a little bit of extra work to remember the mapping (and look up)
  by `(Producer ID, RID)` tuples. The advantage is that leads to smaller numbers
  that can take fewer bytes using VarInt encoding. The downside is it adds more
  complexity on the producer side (either use locks, or some lock-free
  hashtable which is always tricky to get right)

In case of Option 2, we want the RIDs to be scoped either per producer, or per
data-source (but then traced has no notion of data source) but NOT per WriterID.
WriterID (which really map to writer threads) are too granular and get created
dynamically. Mapping RIDs to WriterID would create too much IPC traffic and too
many handshake challenges.

- **Option 3 (current preference)**:
  RIDs are monotonic and scoped per-channel (i.e. per SMB ring buffer / QoS
  channel, discussed at the end of this doc).
  The logic is the following: RIDs can only be used to route packets within
  one SMB ring buffer (if it wasn't the case, then when you want to write a
  packet, which of the N buffer would you write into, and why?).
  This means that on the producer side, we can maintain a table of RIDs per
  ring buffer.

> NOTE on the registry: both Option 2 and Option 3 still require a producer-side
> registry keyed by the stream identity (see config divergence below), in order
> to reuse the same RID for the same stream across sessions. That registry, and
> its concurrency control, is the actual cost. Per-channel scoping (Option 3)
> only changes the size of the integers, not the existence of the registry.
> Option 1 is the only one that avoids the registry entirely. The choice between
> them is essentially "smaller VarInts" vs "no registry / stateless assignment"
> and should be re-evaluated once we know how often a RID actually changes on the
> wire (see the data plane note on RID locality).

### NotifyDataSourceStarted (producer -> traced)

Today every producer must acknowledge the starting of a data source with the
IPC [NotifyDataSourceStarted] on the `producer_port.proto`.

The plan here would be to piggy back on the `NotifyDataSourceStarted` message
to publish the mappings of `RID -> buffer ID` to traced.

Each data source would only emit the mapping for the one data source instance
being started.

Traced internally will have to maintain the merged map of
`RID -> array<Buffer ID>` and keep it updated as data sources are
started/stopped.

When a data source instance stops, traced removes its `(RID -> buffer)` entries.
The lifetime of a RID (and, as we'll see, of the channel and writers backing it)
is therefore refcounted across all sessions that use it, and is decoupled from
any single session.

## Data plane

The idea is to prepend each message in the ring buffer (TracePacket, or
TrackEvent) with the routing id. The specifics of the serialization in the
ring buffer are delegated to [RFC-0014][rfc14].

Conceptually we need one RID per message, we cannot piggy back on
the chunk header or rely on the WriterID, as the same writer will write
events for different categories/ftrace events, so different events in a chunk
can be routed to different buffers.

The RID can of course be optional, avoiding to waste SMB space for producers
that don't support routing (in which case routing would be based purely on
the WriterID / Buffer ID).

> NOTE on RID locality: although conceptually there is one RID per message, in
> practice a writer tends to emit runs of the same RID (e.g. a thread emitting a
> burst of "net" events). We can exploit this by treating the RID as a "current
> routing context" that is only re-emitted when it changes, rather than carried
> verbatim on every message. This amortizes the per-message overhead close to
> zero and makes the wire cost of the RID largely independent of the assignment
> scheme. To be detailed alongside the SMB encoding in RFC-0014.

## Demultiplexing and sequence state

This is the most subtle part of the design and the one with the deepest
consequences for both producers and traced.

### Writers bind to channels, not to sessions

Today a `TraceWriter` is bound to a data-source *instance*, i.e. to a session.
In the new model a routing-aware data source binds its writers to a **QoS
channel** (i.e. an SMB ring buffer) instead. Concretely, for traced_probes,
today we have one `FtraceController` and N `FtraceDataSource`, each with its own
`TraceWriter`. That changes: instead of one writer per instance, we create one
writer per channel.

The axis that collapses is the *instance/session* axis. The *per-thread* axis
must remain, because writers are per-thread (TLS) precisely to keep the hot path
lock-free. So writers become per **(thread × channel)**, replacing today's per
**(thread × instance)**. RFC-0014's ring buffer is already multi-writer, so many
per-thread sequences sharing one channel ring buffer is consistent with it.

The important consequence is architectural: **the write path no longer knows
what a session is**. It knows only channels, RIDs, and whether a RID currently
has any subscriber (a refcount it reads from the SMB, with no IPC). All
session-awareness — fan-out, per-session config, lifetime — lives exclusively in
traced, on the read side of the demux. This severing is what makes routing
coherent.

### The sharding problem

Because a single writer sequence is now emitted once and fanned out by RID to
multiple destination buffers, the sequence is **sharded**: each destination
buffer sees only the subset of the sequence's packets whose RID routed to it.

This collides with the fact that, today, a lot of per-sequence state assumes the
consumer sees the whole, contiguous sequence. We must split that state into two
kinds:

1. **Replicable (absolute / sticky) state.** `TracePacketDefaults`, default
   track uuid, default clock, `TrackEventDefaults`. These do not depend on which
   packets landed where; they are the same for every destination. The producer
   emits them once per (thread × channel) sequence exactly as today, and traced
   **replicates** them into each destination buffer the first time that sequence
   appears there (re-stamping on change). This is pure replication, producer
   unchanged.
   This requires flagging "incremental" state packets at the protocol level
   (which we will need anyways to implement the idea that @primiano and
   @LalitMaganti discussed offline a while ago, which requires another RFC).

2. **Non-replicable (relative / incremental) state.** Anything encoded as a
   delta against the *previous packet in the sequence*: delta timestamps,
   thread-time deltas, counter `CounterIncrementalBase` deltas, interned `iid`
   references. These **cannot** be replicated, because after sharding each
   destination sees a different subsequence, so "the previous packet" is a
   different packet per destination.

The precise rule is therefore: **a producer must never encode a relative value
across a RID boundary**, because the RID boundary is exactly the cut line of the
demux.

### Resolution: self-contained producers, traced regenerates relative encoding

We resolve this by making the producer emit **self-contained** packets and
moving *all* relative/incremental encoding into traced's demux/compaction stage.
This is the natural generalization of RFC-0014's "interning → compression in
traced": now it is interning **and** delta-encoding that migrate to traced,
because only traced knows the final per-destination ordering. The compaction
stage becomes the single place where all space-optimization happens, and the
producer becomes a dumb, session-agnostic firehose.

"Self-contained" here means **self-contained at chunk granularity**, not a fat
absolute timestamp on every event. A relative chain is allowed as long as traced
can resolve it to absolute at *read* time, before fan-out; the only forbidden
thing is a relative encoding that requires data which routed to a different
destination. Within a single chunk traced reads everything, so a chunk-local
incremental chain is fine even across RIDs. Concretely, RFC-0014's "first
timestamp full, rest incremental" stays, with one constraint: **the incremental
chain resets at every chunk boundary** (each chunk restarts with a full
timestamp). For example, given a chunk with:

```
e1 (RID_a, t=100 full)
e2 (RID_b, t=+5)
e3 (RID_a, t=+3)
```

traced reads the chunk, resolves the chain to absolute (100, 105, 108), then
shards: destination of RID_a gets {100, 108}, destination of RID_b gets {105}.
On output, traced re-deltifies per destination. The SMB stays compact; the
shard is correct.

### What traced's demux owns

Per output sequence (one per input-writer × destination buffer), traced:

1. Resolves the chunk-local incremental chains to absolute at read time.
2. Fans each resolved packet to its RID's destination buffers.
3. Per destination: re-deltifies timestamps/counters, assigns the output
   sequence id, computes contiguity / `previous_packet_dropped` (a drop in the
   channel ring buffer is propagated to every destination the lost data would
   have reached), replicates sticky defaults/clock, and finally compresses.

The consumer, at the end, sees ordinary, fully self-contained sequences and is
entirely oblivious to routing.

## Edge cases

### Config divergence breaks naive dedup

Two sessions can request the same logical stream (e.g. category `net`) but with
**different** `TrackEventConfig` (or `FtraceConfig`) options that change the
bytes emitted — e.g. one sets `filter_debug_annotations`, the other doesn't; one
enables `enable_thread_time_sampling`, the other doesn't. If both collapse to the
same RID, they get a single emission with identical bytes, which is wrong for one
of them.

The fix follows directly from the fact that RID derivation is owned by the data
source and opaque to traced: **the data source must fold the byte-affecting
subset of the config into the RID identity**, e.g.

```
RID = HASH(stream_name, byte_affecting_config_fingerprint)
```

(or, for the counter-based options, use that tuple as the registry key). Then
two sessions with *compatible* config dedup to one RID and one emission; two
sessions with *incompatible* config naturally get *different* RIDs, are emitted
separately, and traced routes each to its own subscribers. This degrades
gracefully to today's behaviour for exactly the diverging subset, with no special
case in traced.

The point to call out for implementers: when assigning a RID, the data source
must key it not just on the stream name (category / trace point) but on every
config option that affects the emitted bytes. Getting this wrong silently
corrupts the diverging session.

### RIDs are assumed known at data source start

The control-plane design assumes that all RIDs for an instance are known at
`StartDataSource` time, when the producer walks the enabled trace points /
categories and publishes the `RID -> buffer` mapping. This rules out defining a
RID *dynamically* mid-trace.

There is one concrete case where this assumption does not strictly hold: track
event categories can be registered at runtime (e.g. when a module loads), and a
session with `enabled_categories: "*"` would conceptually want to capture a
category that did not exist yet at start time. Handling this would require an
incremental `RID -> buffer` update pushed to traced after the data source has
started.

**We explicitly choose to ignore this case for now.** A category registered
after a session started will simply not be captured by that already-running
session. If a real need emerges, it can be supported later via incremental
RID-mapping updates over the existing control-plane channel, without changing
the data-plane design.

## Relationship with QoS channels (details in a separate RFC)

RIDs can only route packets within the scope of a single SMB ring buffer: a
packet is physically written into exactly one SMB ring buffer, and the fan-out
to multiple destination buffers happens downstream, in traced. A RID is therefore
meaningful only within one ring buffer.

This changes the vision of [RFC-0014][rfc14]: rather than "one SMB ring buffer
per traced buffer" (which also has memory-overhead concerns, as it scales with
the number of sessions), we move to **one SMB ring buffer per QoS channel**,
where a channel is a write-side isolation domain decoupled from both sessions and
destination buffers. Routing and demultiplexing operate within the scope of one
QoS channel.

The definition of QoS channels — how many there are, how streams map to them,
and how heavy streams get isolated — is out of scope here and will be addressed
in a dedicated RFC. The only property we rely on in this document is that a
stream's mapping to a channel is a function of the stream (not of the session),
so that the same stream requested by two sessions lands in the same channel and
is genuinely emitted only once.

[NotifyDataSourceStarted]: https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/protos/perfetto/ipc/producer_port.proto 
