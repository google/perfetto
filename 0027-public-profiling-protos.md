# Public Protos for Stack Sampling and Heap Profiling

**Authors:** @LalitMaganti

**Status:** Draft

## Problem

Today, anyone consuming a Perfetto trace containing stack samples or heap
profiling data has to read protos that were not designed to be public API:

* **`PerfSample`** (in `protos/perfetto/trace/profiling/profile_packet.proto`)
  is shaped by the `perf_event_open` syscall. Fields like `cpu_mode`,
  `timebase_count`, `follower_counts`, `kernel_records_lost`,
  `unwind_error`, `sample_skipped_reason` and `producer_event` mix three
  unrelated concerns: the actual observation, the perf transport, and
  producer-side diagnostics. The defaults submessage references
  `PerfEvents.Timebase`, which is also perf-shaped.

* **`ProfilePacket`** (heap profiling) overloads its fields by mode: e.g.
  `self_allocated`/`self_freed` are populated in normal mode while `self_max`
  is populated when `dump_at_max=true`, and they are not all set
  simultaneously. It also leaks producer health (`ClientError`,
  `buffer_overran`, `buffer_corrupted`, `hit_guardrail`, `from_startup`,
  `rejected_concurrent`) into the same message as the data. The aggregated
  shape is the only one practically usable; `StreamingAllocation`/
  `StreamingFree` exist but lack callstacks and are documented as "only for
  local testing".

The consequence: the proto schema is tied to implementation details of how
Perfetto records this data today. We cannot evolve the recording side
(e.g. add new sampling backends, support self-emitting profilers, support
async-runtime-aware profiling) without either breaking consumers or growing
the existing protos with more mode flags.

Additionally, these protos do not naturally accommodate **self-emitted**
profilers - profilers that run inside the process being observed (e.g. a
language runtime sampling its own coroutines). The existing shape assumes an
external observer with kernel-level identity (`pid`/`tid`/`cpu`), which is
not always the right model.

We need a new set of protos that:

1. Have **semantic stability forever** - consumers can be written against
   them and not break.
2. Are **transport-neutral** - no `perf_event_open` concepts leaking through.
3. Support **both OS-level profilers** (traced_perf, eBPF samplers,
   simpleperf) **and self-emitted profilers** (runtime samplers, async-aware
   in-process profilers).
4. Cleanly separate **observation data** from **producer diagnostics**.

## Decision

Pending.

## Design

### Scope

This RFC proposes new public protos for two distinct data classes:

* **Stack sampling**: point-in-time observations of where a thread (or
  coroutine, or other execution context) was, along with the value of a
  primary counter and optional follower counters.

* **Heap profiling** in **streaming** form: per-event allocation and free
  records. Heap *snapshots* (point-in-time set of live allocations, akin to
  Java/ART heap dumps) are intentionally out of scope for v1 and will be a
  follow-up; see Open Questions.

The protos live under `protos/perfetto/trace/profiling/` but the load-bearing
surface is the new top-level fields added to `TracePacket`.

### Top-level shape on `TracePacket`

Three new fields on `TracePacket`:

```proto
optional StackSample      stack_sample      = N;
optional HeapAllocation   heap_allocation   = N + 1;
optional HeapFree         heap_free         = N + 2;
```

Each event is its own `TracePacket`. Granular fields (rather than a single
wrapped `ProfilerSample`) were chosen for clarity and because the two data
classes have genuinely different shapes; see Alternatives.

No `TracePacketDefaults` additions. Samples are self-describing through
interned descriptors (`CounterDescriptor`, `HeapDescriptor`, etc.) rather
than via sequence-scoped defaults. This avoids the "samples are
uninterpretable without the defaults packet" hazard and keeps each sample
locally interpretable from `InternedData` alone.

### Task context and execution context

The "who" and "where" of a sample are split into two independently
inline-or-interned messages, because they have very different cardinality
profiles and very different update rates:

* **`TaskContext`** identifies the *subject* of the sample - the task
  being observed (pid, tid, async_id). Slowly-varying (per-thread /
  per-coroutine stable); benefits heavily from interning.
* **`ExecutionContext`** describes *how* the task is executing at sample
  time (cpu, mode). Varies per-sample but has very low cardinality
  (cores × modes), so interning is still effective.

```proto
message TaskContext {
  optional uint64 iid       = 1;

  // pid unset => the producer's own process (resolved from the packet
  // sequence's process descriptor).
  optional uint32 pid       = 2;
  optional uint32 tid       = 3;
  optional uint64 async_id  = 4;
}

message ExecutionContext {
  optional uint64 iid       = 1;
  optional uint32 cpu       = 2;
  optional Mode   mode      = 3;
}
```

Every external-profiling event carries a `TaskContext` (inline or
interned). Stack samples additionally carry an `ExecutionContext`. Heap
events do **not** carry an `ExecutionContext` - cpu/mode are not
meaningful for an allocation record.

Interning notes:

* **Self-emit**: typically intern one `TaskContext` per task and reference
  it from every sample. Per-sample overhead becomes a single varint.
* **OS-level**: typically there are O(threads) unique `TaskContext`s and
  O(cores × modes) unique `ExecutionContext`s across the whole trace.
  Both intern very effectively across thousands of samples.

Interning strategy is a producer concern, not a wire-format concern. The
schema supports either inline or interned for both contexts independently.

### Stack sampling

```proto
message StackSample {
  oneof task_context_field {
    TaskContext task_context     = 1;
    uint64      task_context_iid = 2;
  }

  oneof execution_context_field {
    ExecutionContext execution_context     = 3;
    uint64           execution_context_iid = 4;
  }

  oneof callstack_field {
    Callstack callstack     = 5;
    uint64    callstack_iid = 6;
  }

  // Free-form human-readable hint when the stack is partial or unwinding
  // failed. Consumers MUST NOT pattern-match on the string contents.
  oneof unwind_error_field {
    string unwind_error     = 7;
    uint64 unwind_error_iid = 8;
  }

  // The primary counter is the sampling timebase. Set exactly one of
  // primary_descriptor / primary_descriptor_iid (inline vs interned).
  optional CounterDescriptor primary_descriptor     = 9;
  optional uint64            primary_descriptor_iid = 10;
  optional uint64            primary_weight         = 11;

  // Follower counters (e.g. instructions, cache_misses read at the same
  // sample). Populate EITHER follower_descriptors OR
  // follower_descriptor_iids per sample. follower_weights is positional
  // with whichever is set.
  repeated CounterDescriptor follower_descriptors     = 12;
  repeated uint64            follower_descriptor_iids = 13 [packed = true];
  repeated uint64            follower_weights         = 14 [packed = true];
}
```

### Heap profiling (streaming)

```proto
message HeapAllocation {
  oneof task_context_field {
    TaskContext task_context     = 1;
    uint64      task_context_iid = 2;
  }

  oneof callstack_field {
    Callstack callstack     = 3;
    uint64    callstack_iid = 4;
  }

  oneof heap_field {
    HeapDescriptor heap     = 5;
    uint64         heap_iid = 6;
  }

  // Raw allocation size in bytes; NOT sampling-corrected. The
  // HeapDescriptor declares the sampling interval; consumers compute
  // corrected estimates themselves.
  optional uint64 size = 7;

  // Optional. Pairs this allocation with its HeapFree for per-alloc
  // lifetime tracking. Omitted by producers that do per-callsite
  // aggregation only.
  optional uint64 alloc_id = 8;
}

message HeapFree {
  oneof task_context_field {
    TaskContext task_context     = 1;
    uint64      task_context_iid = 2;
  }

  // The ALLOC-SITE callstack (denormalized by the producer from its
  // address->callstack map), NOT the free-site callstack. This lets
  // consumers do per-callsite aggregation with no state-keeping.
  oneof callstack_field {
    Callstack callstack     = 3;
    uint64    callstack_iid = 4;
  }

  oneof heap_field {
    HeapDescriptor heap     = 5;
    uint64         heap_iid = 6;
  }

  optional uint64 size     = 7;
  optional uint64 alloc_id = 8;
}
```

### Descriptors

Each descriptor type is its own message and follows the same inline-or-
interned pattern. Inline is for simplicity / low-volume cases; interned
(via `InternedData`) is for high-rate cases.

```proto
message CounterDescriptor {
  optional uint64 iid          = 1;
  optional string name         = 2;  // e.g. "cycles"
  optional Unit   unit         = 3;
  optional string unit_str     = 4;  // free-form override for custom counters
  optional string description  = 5;
}

message HeapDescriptor {
  optional uint64 iid                     = 1;
  optional string name                    = 2;  // e.g. "malloc", "Skia"
  optional uint64 sampling_interval_bytes = 3;
  optional string description             = 4;
}

message AsyncContextDescriptor {
  optional uint64 iid        = 1;
  optional string name       = 2;
  optional string kind       = 3;  // e.g. "goroutine", "fiber"
  // Structural parent (the task that spawned this one). NOT causality:
  // dynamic causality (await chains, wakeups) is modelled via TrackEvent
  // flows, not here.
  optional uint64 parent_iid = 4;
}
```

`InternedData` is extended with:

```proto
repeated TaskContext             task_contexts              = X;
repeated ExecutionContext        execution_contexts         = X + 1;
repeated CounterDescriptor       counter_descriptors        = X + 2;
repeated HeapDescriptor          heap_descriptors           = X + 3;
repeated AsyncContextDescriptor  async_context_descriptors  = X + 4;
```

`unwind_error_iid` reuses the existing interned string tables.

### Standardized enums

```proto
enum Mode {
  MODE_UNKNOWN      = 0;
  MODE_USER         = 1;
  MODE_KERNEL       = 2;
  MODE_HYPERVISOR   = 3;
  MODE_GUEST_USER   = 4;
  MODE_GUEST_KERNEL = 5;
}

enum Unit {
  UNIT_UNSPECIFIED        = 0;
  UNIT_NANOSECONDS        = 1;
  UNIT_CPU_CYCLES         = 2;
  UNIT_INSTRUCTIONS       = 3;
  UNIT_BYTES              = 4;
  UNIT_PAGE_FAULTS        = 5;
  UNIT_CACHE_MISSES       = 6;
  UNIT_CACHE_REFERENCES   = 7;
  UNIT_BRANCH_MISSES      = 8;
  UNIT_COUNT              = 9;  // dimensionless
}
```

Both enums are **append-only forever**. Producers that need a unit not in
the enum use `unit_str` on the `CounterDescriptor`. New `Unit` values are
added only when broadly useful.

### Callstack model

We reuse the existing `Callstack` / `Frame` / `Mapping` model in
`profile_common.proto` for the interned form, and mirror `TrackEvent`'s
inline `Callstack` (function_name / source_file / line_number) for the
inline form. No new callstack representation is introduced.

For out-of-band sampling profilers, the **interned** form is the natural
mode (raw PCs + mappings, offline symbolization). The inline form is for
synthetic / low-volume / test cases where simplicity outweighs efficiency.

### Semantic stability commitments

These protos commit to forever-stable semantics, not just wire stability.
Concretely:

* **No overloaded fields.** Every field has exactly one meaning regardless
  of other field values or defaults. The `self_max`/`self_allocated`
  mode-flip in the current `ProfilePacket` is exactly the pattern we will
  not repeat. New states get new fields.
* **Enums grow append-only.** `Mode` and `Unit` (and any future enum)
  never repurpose existing values.
* **Defaults declare units and labels, never meaning.** Defaults can say
  "the primary counter measures CPU cycles" but never "field X means Y
  when default Z is set".
* **Diagnostics live elsewhere.** Producer health signals (data loss in
  ring buffers, unwinder failures, guardrail trips, client errors) are not
  in this proto. They go in a separate sidecar packet defined in a follow-
  up RFC. The only producer-side hint we keep on the sample itself is
  `unwind_error`, which describes the *data* (the stack is partial), not
  the producer's internal state.

### Relationship to existing protos

The existing `PerfSample`, `ProfilePacket`, `StreamingAllocation`,
`StreamingFree` and `StreamingProfilePacket` are not deleted, deprecated,
or affected by this RFC. They continue to work and continue to be the
emission format for heapprofd and traced_perf. There is no goal of
migrating those producers to the new protos. The new protos are aimed at
new producers - in particular OS-level samplers other than
`perf_event_open` and self-emitting in-process profilers - that have no
incumbent format and need a stable public surface to target.

### Relationship to `TrackEvent`

These protos are deliberately **not** part of `TrackEvent`. `TrackEvent`
is in-band instrumentation ("this thing happened in my program, here's a
callstack of where"). External / out-of-band profiling is a separate
concept, sibling to `TrackEvent` and to the generic kernel-event protos.

The two concepts share the `InternedData` interning model (callstacks,
frames, mappings), but the message shapes do not depend on each other.

## Alternatives considered

### A. Single unified `ExternalProfileSample` message with a oneof payload

```proto
message ExternalProfileSample {
  oneof payload {
    StackSample      stack       = 1;
    HeapAllocation   heap_alloc  = 2;
    HeapFree         heap_free   = 3;
  }
}
```

Pro:

* Single `TracePacket` field to extend later.

Con:

* Forces stack and heap to share an envelope when they are genuinely
  different shapes.
* Adds a layer of message nesting for every event with no semantic gain.
* Premature unification - if a future sample type does share a shape with
  these, it can join the family without retroactively requiring the
  wrapper.

We prefer granular `TracePacket` fields.

### B. Extend `TrackEvent` rather than introducing new top-level protos

Pro:

* Reuses existing track infrastructure, identity, clocks, UI integration.

Con:

* `TrackEvent` is in-band instrumentation. External profilers are *not*
  the instrumented program describing itself; they are an external (or
  internal but out-of-band) observer describing the program. Conflating
  these has the same shape as conflating `ftrace` events with `TrackEvent`
  - which we already do not do.
* `TrackEvent`'s per-event overhead (event name, debug annotations, track
  reference) is unnecessary at kHz-scale sampling rates.

### C. Window-aggregated heap profile (preserving today's `ProfilePacket` shape)

Pro:

* Smaller on the wire.
* Producers that aggregate naturally (some Java/ART tooling) fit directly.

Con:

* Aggregated semantics are where most of the existing proto's footguns
  live (`dump_at_max` mode flip, `continued` chunking, peak vs window
  ambiguity).
* Aggregates can be derived from streaming events; the reverse is not
  true.
* Heap dumps (point-in-time snapshots) are a separate concept that
  deserves its own message, not a third mode of the same one.

We commit to streaming-only in v1; a future heap-snapshot proto will be a
distinct message.

### D. Batched / columnar (SoA) per-packet shape

Pro:

* Significantly smaller wire size for hot paths.
* Existing `StreamingAllocation` / `StreamingProfilePacket` already use
  this shape.

Con:

* Harder to extend (every new field is another parallel array).
* Loses per-field optionality without sentinel values.
* Less ergonomic for consumers.

Decision: AoS (one message per event) is the canonical public shape. If
measured to matter, a sibling batched message can be added as a future
optimization without disturbing the canonical shape.

## Open questions

* **Heap snapshots / dumps.** A point-in-time "set of currently-live
  allocations per callsite" is semantically distinct from streaming
  alloc/free and from a window aggregate. It also has natural overlap with
  Java/ART `heap_graph.proto`. Out of scope for this RFC; follow-up.

* **Stackless coroutines.** C++20 / Rust async coroutines do not have a
  contiguous stack; their "callstack" is an await chain / state machine.
  v1 supports stackful coroutines (goroutines, Boost.Context, fibers)
  using the existing `Callstack` model. Stackless coroutine modelling is
  deferred.

* **Producer diagnostics sidecar.** What does the new "producer health"
  packet look like (data loss, unwinder errors, guardrails)? Separate
  follow-up RFC.
