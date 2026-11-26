# In-kernel GPU events

**Authors:** @dreveman

**Status:** Draft

## Problem

A GPU trace today stops at the kernel boundary. A kernel launch is one
`GpuRenderStageEvent` slice on a queue track, and GPU counters sample the whole
device or a hardware block. That matches how most kernels are written, but a
growing class of kernels moves scheduling *into* the kernel: persistent kernels,
warp-specialized kernels and, at the far end, megakernels that run a whole model
step in one launch. For those, the interesting behavior happens inside a single
slice, and the trace cannot show it. The background below explains these
patterns for readers who don't work with them day to day.

### Background: persistent kernels, warp specialization and megakernels

**The ordinary model.** A kernel launch creates a grid of thread blocks (CTAs on
NVIDIA, workgroups on AMD), usually many more than there are SMs or CUs. The
hardware dispatches blocks to SMs as earlier ones retire; each block is
short-lived and independent of the others. The kernel ends when its last block
retires, and the next kernel on the stream starts after that. Because every
block does the same thing, a slice per launch plus some counters describes such
a kernel well.

**Persistent kernels.** A persistent kernel launches roughly one block per SM
and keeps it resident: each block loops over many work items (e.g. output tiles
of a GEMM), taking them from a static schedule or a queue in global memory,
until the work runs out. This amortizes block launch and prologue costs, lets
the kernel balance work itself instead of relying on the hardware dispatcher,
and keeps state such as shared memory buffers alive across work items. The idea
goes back to "persistent threads" ([Gupta et al., 2012](#references)); today it
is the default structure of high-performance GEMMs, e.g. CUTLASS's persistent
kernels with a tile scheduler
([CUTLASS](https://github.com/NVIDIA/cutlass/blob/main/media/docs/cpp/efficient_gemm.md))
and Stream-K work decomposition ([Osama et al.,
2023](https://arxiv.org/abs/2301.03598)).

**Warp specialization.** Within a block, warps can take different roles instead
of all running the same code. In the common producer/consumer form, a producer
warp issues asynchronous copies (TMA on NVIDIA Hopper and Blackwell) of the next
tiles into a ring of shared memory buffers, consumer warps run tensor core math
on the tiles that have arrived, and the two sides hand buffers back and forth
through barriers. Loads then overlap compute instead of alternating with it. The
pattern dates back to CudaDMA ([Bauer et al., 2011](#references)) and is central
to CUTLASS's Hopper and Blackwell kernels
([CUTLASS](https://github.com/NVIDIA/cutlass/blob/main/media/docs/cpp/efficient_gemm.md))
and to FlashAttention-3, which also "ping-pongs" two consumer warp groups so one
does softmax while the other does matrix multiplies ([Shah et al.,
2024](https://arxiv.org/abs/2407.08608)). Other hardware reaches the same
overlap differently: HipKittens on AMD uses 8-wave ping-pong and 4-wave
interleave schedules instead of producer/consumer waves ([HipKittens,
2025](https://arxiv.org/abs/2511.08083)). Either way, the warps (or waves) of
one block do different things at different times.

**Megakernels.** A megakernel takes persistence to a whole model step: one
launch runs many operators, e.g. every layer of an LLM decode step, the LM head
and sampling. Each resident block runs an interpreter loop over a list of
instructions or a task queue, and dependencies between operators are tracked
with counters in global memory instead of kernel boundaries. The motivation is
small-batch LLM decode: it is memory-bandwidth bound, and each operator is only
a few microseconds long. With one kernel per operator, every boundary leaves the
GPU partly idle while the last blocks of one kernel drain and the next kernel
ramps up, and the next operator's weights are not streaming in the meantime.
CUDA graphs and programmatic dependent launch shrink these gaps but keep the
boundaries. A megakernel removes them: an SM that finishes its part of one
operator moves on, and can start streaming the next operator's weights while
others finish. Hazy Research's hand-written Llama-1B decode megakernel (["No
Bubbles", 2025](https://hazyresearch.stanford.edu/blog/2025-05-27-no-bubbles))
and the MPK compiler, which turns a model into a persistent kernel with an
in-kernel runtime ([Cheng et al., 2026](https://arxiv.org/abs/2512.22219)), are
two public examples. Published megakernels mostly target decode: prefill is
compute bound, gains less from removing kernel boundaries and typically runs as
ordinary kernels. The Hazy Research demo, for example, runs prefill through the
regular PyTorch model before handing decode to the megakernel. (The term is
older: ray tracers used it for single-kernel path tracers ([Laine et al.,
2013](#references)).)

The two models look like this on a timeline:

```text
Ordinary kernels: one slice per operator, a boundary between each pair

  queue  [norm][ qkv ][ attn ][ o ][norm][  gate_up  ][ down ][norm][ qkv ]...

Decode megakernel: one slice per step; the schedule lives inside it

  queue  [ decode_megakernel ................................................]
  SM 0     [qkv][ attn ][o][ gate_up ][down][qkv][ attn ][o][ gate_up ]...
  SM 1     [qkv][ wait ..][o][ gate_up ][down][qkv][ wait ..][o][ gate_ ]...
            ^ warps inside each SM: controller (fetch, wait, issue loads),
              compute (wait for data, compute), loads in flight
```

**What this means for profiling.** In all three patterns the kernel becomes a
small runtime. Work assignment, dependency waits, load/compute overlap,
imbalance between SMs and the tail at the end of a step all happen inside one
launch. Launch-level data (one slice per launch, counters aggregated over the
kernel, sampled program counters) cannot attribute time to an operator inside a
megakernel or show which SM waited for which dependency. Kernel authors
therefore build in-kernel timing into their kernels; the Hazy Research
megakernels, for example, record timestamps per instruction and per warp role on
every SM and render them as a timeline.

### Existing in-kernel instrumentation

In-kernel instrumentation exists in many forms:

- Source-level timestamp buffers written by the kernel itself (e.g. the
  FlashInfer profiler).
- Compiler-integrated markers (e.g. Triton Proton's intra-kernel mode, CuTe DSL
  in-kernel event tracing).
- Binary instrumentation that rewrites the compiled kernel (NVBit-style SASS
  rewriting).
- Megakernel frameworks' own per-instruction timing (e.g. the Hazy Research
  Megakernels `.mkprof` timing slots).
- Hardware or firmware event tracing on accelerators.

Each of these produces its own output: Chrome JSON or a one-off Perfetto trace
built with ad hoc helpers, with its own track layout and its own clock. None of
them is aligned with the host-side trace of the same run (e.g. CUPTI-based
profilers such as Kineto) or mergeable with other GPUs and processes.
FlashInfer, for one, moved from Chrome JSON to Perfetto's format only because
Chrome JSON cannot represent overlapping asynchronous ranges on one thread.

### Who records in-kernel data

In-kernel data rarely comes from the producer that records the kernel launches.
It comes from an instrumentation library or a decoder, often after the run and
often into a separate file, while a CUPTI- or rocprofiler-based profiler records
the launches. Combining the two needs identities that both producers can name
without sharing track uuids.

The data also belongs to the process that launched the kernels. Another process
on the same GPU may record similar data at the same time, and neither producer
knows about the other. Machine-wide tracks, such as per-SM tracks under the GPU,
would interleave the two processes' kernels on the same tracks, and could even
overlap them when the GPU is shared spatially.

What is missing is therefore not another recorder but a convention: how decoded
in-kernel events extend the traces of a run, placed in the logical domain of the
process that launched the kernels (its logical devices and queues), so that
trace processor and the UI treat them as part of the GPU timeline independent of
the vendor (CUDA, HIP, other accelerators), the recording scheme and the
producer.

### Goals and non-goals

Goals:

1. A vendor- and recorder-neutral representation built from existing
   primitives: `TrackEvent`, `GpuTrackDescriptor`, `ClockSnapshot`, flows and
   interning.
2. Placement in the logical domain of the process that launched the kernels,
   with identities that any producer and any trace file can attach to.
3. Kernel-wide progress on the logical queue the kernel was launched on.
4. Correct alignment of device timestamps with the rest of the run's traces.
5. Best practices for producing trace files with in-kernel events.

Non-goals:

- Device-side recording APIs, buffer formats or decoders. Every recorder keeps
  its own; this RFC starts where a recorder has decoded its buffers.
- Vendor-specific schemas or a fixed vocabulary of hardware levels.
- How `logical_queue_id` values are assigned. Logical queues are specific to
  each API, so each API defines its own convention.
- Machine-wide views of in-kernel data and hardware queues, and links from
  in-kernel events to individual kernel launches; see
  [Future work](#future-work).
- New UI visualizations beyond placing the tracks correctly.

## Decision

Pending

## Design

### Overview

A decoded in-kernel event has a location (a hardware block, then
producer-defined levels such as warps or roles), a timestamp in a device clock,
a kind (range, marker, counter value), optional arguments, and optional links to
other events. Everything hangs off the logical device of the process that
launched the kernel, and each piece maps onto something Perfetto already has:

| Concept                                  | Representation                                                |
| ---------------------------------------- | ------------------------------------------------------------- |
| Process                                  | `TrackDescriptor` with `process.pid`                          |
| Logical device (the process's GPU)       | Descriptor under the process with `gpu_track.gpu_id`          |
| Logical queue (e.g. a CUDA stream)       | Descriptor with `gpu_track.logical_queue_id`                  |
| Hardware block (SM, CU, PE) or grouping  | Descriptor under the device with `gpu_track.gpu_domain_id`    |
| Deeper levels (warp, wave, core, role)   | Child descriptors, inheriting GPU and domain                  |
| Ranges and markers                       | `TrackEvent` slices and instants                              |
| Overlapping asynchronous ranges          | Same-named sibling tracks, merged by name                     |
| Per-location values                      | Counter `TrackDescriptor`s                                    |
| Dependencies, data movement, progress    | `TrackEvent` flows                                            |
| Kernel-wide progress (stages of a launch)| Track declaring the launch's logical queue                    |
| Device timestamps                        | Sequence-scoped clock in `ClockSnapshot`                      |

`GpuTrackDescriptor` (`gpu_id`, `hw_queue_iid`, `logical_queue_id` and
`gpu_domain_id`) landed in [#8742](https://github.com/google/perfetto/pull/8742),
[#8761](https://github.com/google/perfetto/pull/8761) and
[#8763](https://github.com/google/perfetto/pull/8763); trace processor and UI
support for it is in progress. This RFC needs no proto changes. It relies on
logical queues binding per process and on descriptors that declare the same
identity merging; see [Identities and merging](#identities-and-merging) and
[Trace processor and UI](#trace-processor-and-ui).

For an NVIDIA-shaped producer that instruments four SMs of a decode megakernel:

```text
inference_server              process { pid }
└── CUDA                      grouping chosen by the producer
    └── Device #0             gpu_track { gpu_id: 3 }            logical device
        ├── Stream #2         gpu_track { logical_queue_id }     kernel slices
        ├── Kernel progress   gpu_track { logical_queue_id }     merges with Stream #2
        └── SMs
            ├── SM #0         gpu_track { gpu_domain_id: 0 }     lifetime, tasks
            │   ├── Queued tasks      counter
            │   ├── Controller        warp 0: fetch, wait deps, issue
            │   │   ├── Async copies  overlapping loads, slot 0
            │   │   └── Async copies  overlapping loads, slot 1
            │   ├── Compute #1        warp 1
            │   └── Compute #2        warp 2
            └── SM #1 ...
```

Here the process's device 0 (its first visible GPU) is the machine's GPU 3:
`gpu_id` is the machine-wide ID, the name is the process's.

An AMD-shaped producer uses `SEs -> SE #n -> CU #m` with role-named waves below
under its device, an accelerator with processing elements `PEs -> PE #n`,
without anything else changing.

![A synthetic inference trace in the Perfetto UI. At the top, the GPU's
machine-wide view with its counters and hardware queues. Below, the process's
view: CUDA, Device #0, Stream #2 with the prefill kernels under a prefill
annotation followed by four decode megakernels, and the SMs with each SM's
tasks, queued-task counter, controller warp, async copies and compute
warps.](media/0054/overview.png)

_A synthetic inference trace (a prefill with ordinary kernels, then four decode
steps of one megakernel each) with in-kernel events for four SMs, rendered with
a prototype of the trace processor and UI behavior described in this RFC. The
hardware queues show the global view of the GPU; the process's `Stream #2` shows
its kernels, and `SMs` the in-kernel detail._

### Identities and merging

Three identities can be named by any producer without sharing track uuids:

- **Process:** the pid (on its machine). This works today.
- **Logical device:** the process and a `gpu_id`, declared by the highest
  descriptor under the process that sets `gpu_track.gpu_id`. The `gpu_id` is
  the machine-wide GPU ID used by render-stage events, GPU counters and
  `GpuInfo` (where it is the index into the list of GPUs), not the API's device
  ordinal. Ordinals depend on what each process can see (e.g.
  `CUDA_VISIBLE_DEVICES`), so the same GPU can be device 0 in one process and
  device 3 in another, while its `gpu_id` stays the same. The ordinal belongs in
  the descriptor's name (`Device #0`).
- **Logical queue:** the process and a `logical_queue_id`. How the IDs are
  assigned is specific to each API; a producer that attaches to a queue uses
  the same convention as the producer of the queue's render-stage events.

We propose that descriptors declaring the same logical device or logical queue
merge into one track, whichever producer or file they come from and whatever
their uuids, names or intermediate parents. Their descendants attach below the
merged track, and the merged track is placed where the other descriptors with
that identity place it. The display names and groupings in the tree above
(`CUDA`, `Device #0`, `Stream #2`) are the producer's; only the identities
matter.

In-kernel events, including kernel-wide progress, are limited to this logical
hierarchy: nothing goes on tracks tied to `hw_queue_iid` or elsewhere in the
GPU's machine-wide tree. Only render-stage events, which describe the kernels
themselves, appear in both views.

`gpu_id` and `gpu_domain_id` also remain track dimensions, so location tracks
stay comparable across processes and with machine-wide hardware counters in SQL
and the UI, without sharing a tree.

### Location

**One domain per path.** Under the logical device, the first level that
corresponds to a hardware block (or a producer-defined grouping) carries
`gpu_domain_id`. Use the same IDs that hardware counters use in
`InternedGpuCounterDescriptor.gpu_domain_id`. Only one level of a path can be a
domain; with nested hardware levels (e.g. shader engine, then CU) pick the
level counters are reported at and make the others plain descendants.

**Workers first, then roles.** Nest by the unit that schedules work, with the
warps, waves or cores it contains below it. For persistent kernels and
megakernels that unit is the persistent worker: an SM, CU or PE, usually running
one resident block.

- The worker's track is its task timeline, which is where bubbles, load
  imbalance and dependency stalls show. A collapsed worker still shows it.
- Roles inside a worker are mostly compared with each other (a loader running
  ahead of its consumers, a controller waiting on dependencies), so they sit
  next to each other.
- It scales: a hundred or more workers with a few roles each stay readable as
  collapsible groups.
- Per-block hardware counters line up with the worker tracks through the domain.

Existing tools lay data out the same way. The Hazy Research Megakernels viewer
shows one row per SM, with each warp role's timestamps inside the instruction.
The FlashInfer profiler nests warp groups under SM and block, and CuTe DSL's
in-kernel event tracing groups tracks by CTA and warp.

Comparing one role across all workers (e.g. whether every loader stalls at the
same time) is a regrouping of the same tracks, not a second layout. With the
same role names on every worker and the domain dimension, it is a query (see
[Trace processor and UI](#trace-processor-and-ui)), and the UI could offer it as
a pivot through track grouping dimensions
([RFC-0039](0039-track-grouping-dimensions.md)).

**Deeper levels are plain children.** Warps, waves, cores and roles are child
`TrackDescriptor`s that inherit the GPU and domain. Recommended conventions:

- Name hardware levels `<Level> #<index>` and set `sibling_order_rank` to the
  index with `child_ordering = EXPLICIT` on the parent, so `SM #10` sorts after
  `SM #9`.
- Name warps and waves with fixed roles by role (e.g. `Controller`, `Loader`,
  `Consumer #1`), the same on every worker, and give the hardware index and
  what the role does in `description` (e.g. "Warp 0: fetches tasks, waits on
  dependencies, issues loads"). Interchangeable lanes keep `<Level> #<index>`.
- Emit tracks only for locations that recorded events. Instrumenting a subset of
  SMs is normal and keeps traces small.

**Tracks are per location, not per launch.** A location's tracks persist across
launches. Per-launch structure is expressed with slices: for persistent kernels,
a lifetime slice per launch on the worker's track (named after the kernel) with
the tasks nested inside. Kernels that run concurrently within one process (e.g.
on different queues) can share a location; their overlapping slices go on
additional same-named tracks, as for any overlapping ranges.

### Events

In-kernel events are ordinary `TrackEvent`s and follow the usual rules:

- **Slices and instants.** Slices on one track must nest. Use instants for
  points such as a completion signal.
- **Overlapping ranges.** Asynchronous work that overlaps on one location (e.g.
  double-buffered loads) goes on separate tracks, one per in-flight slot. Give
  the slots the same name and the default `SIBLING_MERGE_BEHAVIOR_BY_TRACK_NAME`
  renders them as one track with lanes.
- **Arguments.** Use debug annotations for payloads such as layer, task id or
  iteration, and intern event names (`EventName`).
- **Flows.** Use flows for data movement within a domain (a load to the compute
  that consumes it) and for dependencies across domains (the task that finished
  last to every task that waited for it). Put the outgoing flow on the slice or
  instant where the producer signals and the terminating flow on the slice where
  the consumer proceeds, so arrows point forward in time.
- **Counters.** Per-location values recorded by the kernel (e.g. queued tasks)
  are counter `TrackDescriptor`s under the location. Hardware counters per block
  stay machine-wide (`GpuCounterEvent` with `gpu_domain_id`) and relate to the
  location tracks through the GPU and domain dimensions.

### Kernel-wide progress

Some progress belongs to a launch as a whole rather than to one location. In a
megakernel, a layer or a stage starts when the first worker picks up one of its
tasks and completes when the last worker finishes, which the kernel observes
when the stage's completion counter reaches its task count. These markers are
the natural coarse view of a persistent kernel: they refine the kernel's slice
on its queue into the stages that every SM works through.

Put them on a track that declares the logical queue the kernel was launched on.
It merges with the queue's track, and its slices nest below the kernel slice
they fall in: the kernel is the outermost range, with its layers and stages
below it, while the queue's own descriptor and render-stage events stay
untouched. In the tree above, that is `Kernel progress`, next to `Stream #2`
under `Device #0`.

![The start of one decode megakernel. On Stream #2, the decode_megakernel slice
with layer 0 to layer 2 nested below it and the qkv, attention, o_proj, gate_up
and down stages below each layer. Below, each SM's lifetime slice with its
tasks and completion signals, and on SM #0 the queued-task counter, the
controller warp, two async copy lanes and two compute
warps.](media/0054/kernel.png)

_The start of one decode megakernel. Kernel-wide progress nests below the
kernel slice on `Stream #2` and lines up with the tasks on every SM; attention
runs on only some of the SMs while the others wait for it._

Recommended structure:

- One slice per stage, from the first worker's start to the last worker's
  completion, optionally grouped (e.g. one slice per layer containing its
  stages).
- Arguments that summarize the stage, e.g. the number of tasks and the domain
  that completed last, which points at the straggler.
- Slices on the queue must nest, with each other and with the kernel slices,
  which holds when progress lies within its launch. If stages of one launch
  overlap (e.g. a pipelined kernel starts the next stage on some SMs before the
  previous one has completed on others), put the overlapping slices on a child
  track of the progress track.
- Host-side annotations on the queue (e.g. a step that spans several launches)
  must nest with the kernels too. A step that is a single launch needs none: its
  kernel slice is already the outermost range.
- Flows from each stage slice to each of the stage's tasks on the worker
  tracks. Because the progress track is an ordinary track event track (unlike
  the queue's render-stage slices), flows can connect it with per-location
  events, so selecting a stage leads to the per-SM work and back.

![The qkv stage of layer 1 selected on Stream #2, with flows to the qkv task on
each of the four SMs. The stage starts when the slowest SM finishes the down
task of layer 0. On SM #0, the controller fetches the next task and waits on
its dependencies while attention runs on SM #2 and SM #3; the compute warps
alternate between waiting for loaded chunks and computing on
them.](media/0054/stage-flows.png)

_Selecting a stage on the queue leads to its tasks on every SM. The stage
starts when the slowest SM finishes the previous one, and SM #0's controller
waits on its dependencies while attention runs elsewhere._

A stage starts exactly when its first task does, so a stage's outgoing flow and
the task's terminating flow can share a timestamp. Write the outgoing end first:
trace processor handles events in timestamp order, in packet order for equal
timestamps, and ignores a terminating flow it has not seen start.

The per-location tracks stay the source of truth. Kernel-wide progress is a
summary that the kernel records from its global progress markers or, if the
recorder only has per-location events, that a decoder derives from them.

### Timestamps and clocks

Device timers differ by vendor and by kind:

| Device timer                                         | How to encode it                                              |
| ---------------------------------------------------- | ------------------------------------------------------------- |
| Global nanosecond timer (e.g. NVIDIA `%globaltimer`) | Custom clock, `unit_multiplier_ns` unset                      |
| Global fixed-rate timer (e.g. AMD `s_memrealtime`)   | Custom clock, `unit_multiplier_ns: 10` for 100 MHz            |
| Per-block cycle counter, or a non-integer ns rate    | Convert to ns in the producer, then encode as above           |

`unit_multiplier_ns` is an integer, and cycle counters are neither synchronized
across blocks nor constant-rate, so producers convert those to nanoseconds
themselves, for example by anchoring each block's counter to the launch window
or to samples of a global timer.

Each packet sequence that carries in-kernel events:

1. Clears incremental state on its first packet and sets
   `TracePacketDefaults.timestamp_clock_id` to a sequence-scoped clock (an ID in
   `[64, 127]`), so events need no per-packet clock ID.
2. Emits a `ClockSnapshot` pairing that clock with `BOOTTIME` (or another clock
   the other traces of the run use) at every sync point, e.g. the start of each
   launch. A file written without a tracing service also includes `REALTIME`;
   see [Multi-machine runs](#multi-machine-runs).
3. Optionally declares the clock `is_incremental` and writes timestamps as
   deltas. Events must then be written in timestamp order.

Trace processor converts a timestamp using the closest preceding snapshot of
its clock: a piecewise constant offset, without interpolation. A snapshot per
launch bounds the error to the drift over one launch. How a producer obtains
the (device, host) pairs is producer-specific: launch timestamps from CUPTI or
rocprofiler, a driver timestamp query, or a short kernel that reads the device
timer.

Clocks in `[64, 127]` are only valid on the sequence that declares them, so a
per-GPU clock shared by several sequences has to be re-declared on each. IDs
from 128 up are reserved for future trace-scoped clocks; see
[Open questions](#open-questions).

### Producing trace files

In-kernel events are usually decoded after the fact, from device buffers or a
recorder's own output, and often by a different tool than the one that recorded
the launches. The basic unit is therefore a self-contained trace file with
in-kernel events that merges with the run's other traces. Such a file:

- is merged with the other traces as an archive (`trace_processor util merge`,
  see [RFC-0016](0016-merged-trace-clock.md)). Do not byte-concatenate
  separately produced traces: sequence IDs and track uuids collide;
- carries its own `ClockSnapshot`s relating each device clock to a clock the
  other traces share (`BOOTTIME` within the same boot), as described in
  [Timestamps and clocks](#timestamps-and-clocks);
- anchors on the process (by pid) and declares the logical devices and queues
  it needs by their identities, directly under the process, instead of
  referencing uuids from another file or mirroring another producer's
  groupings. Declare the process once per file, under one uuid: re-declaring it
  under a second uuid can make trace processor treat it as a new process with
  the same pid;
- uses the same `gpu_id` namespace as the `GpuRenderStageEvent.gpu_id` of the
  run, and is attributed to the same machine (the same `machine_id`, or a
  mapping in the archive's `perfetto_manifest`). In-kernel recorders usually
  know the API's device ordinal, or the device UUID or PCI address, rather than
  the machine-wide ID. Map it by the UUID or PCI address, which `GpuInfo`
  records for each `gpu_id`, not by the ordinal;
- writes each device's events on their own packet sequence, so its device clock
  and interning stay local to it. A file written without a tracing service
  stamps `trusted_packet_sequence_id` on every packet itself, and `machine_id`
  when the run's other traces use one.

The same packets can also be written into the main trace by the tool that
records it. The clock, sequence, identity and volume rules apply either way.


**Identity.** `GpuTrackDescriptor` fields are part of a track's identity: every
descriptor emitted for a uuid declares the same values, and a descendant does
not declare a different `gpu_id` or `gpu_domain_id`.

**Volume.** In-kernel events scale with instrumented locations times launches,
and recorders can produce millions of events per second. Recommendations:

- Instrument a subset of locations and honor capture windows.
- Intern event names and annotation names.
- Use delta timestamps.
- Keep track uuids small: they are written on every event, and a uuid that uses
  all 64 bits (e.g. a hash) takes 10 bytes as a varint.
- Keep constant arguments on the enclosing slice rather than on every event.

#### Multi-machine runs

This is not specific to in-kernel events, but it is easy to miss for producers
that write trace files themselves. A run that spans machines (e.g. one trace per
host of a distributed job) merges into one timeline only if every file:

- **Says which machine it comes from.** Either `TracePacket.machine_id` on every
  packet (non-zero and unique per machine), or a `machine` entry for the file in
  the archive's `perfetto_manifest`. The SDK stamps the ID itself when it is
  initialized with one (e.g. `TracingInitArgs::machine_id` in the C++ SDK), and
  `SystemInfo.machine_name` gives the machine a readable name.
- **Relates its clocks to `REALTIME`.** Trace processor assumes `REALTIME` to be
  in sync across machines (NTP or PTP); `BOOTTIME` is not. A file whose
  snapshots only pair a device clock with `BOOTTIME` can only be placed through
  another file from the same machine. Traces recorded through a tracing service,
  including the SDK's in-process backend, already contain snapshots with
  `REALTIME`; files written directly with protobuf bindings add it to their
  `ClockSnapshot`s themselves.

Without these, trace processor drops the events it cannot place, or puts
several machines' processes and GPUs on one machine. See Perfetto's
[trace merging documentation](https://perfetto.dev/docs/analysis/merging-traces).

### Trace processor and UI

Beyond the GPU track descriptor support that is in progress, this RFC needs the
following behavior:

1. **Logical queues bind per process.** A descriptor that declares
   `logical_queue_id` binds that queue of its process (from its process
   ancestor), across packet sequences and files. `hw_queue_iid`, in contrast,
   only binds within one packet sequence.
2. **Render stages land on their logical queue.** A render-stage event with a
   `logical_queue_id` is placed on the track bound to that queue in the process
   of the event's graphics context. An event that also has an `hw_queue_iid`
   appears on both tracks: the hardware queue is the global view of the GPU,
   the logical queue is the process's view of the same work. This applies to
   render-stage events (the kernels themselves) only; in-kernel events never go
   on hardware queue tracks.
3. **Order does not matter.** Descriptors and render-stage events can arrive in
   any order and from different files of an archive.
4. **Same identity, one track.** Descriptors that declare the same logical
   device or logical queue of a process end up as one track, across producers
   and files, placed with the other descriptors of that identity. On a logical
   queue, their slices and the queue's render stages nest by time, so
   kernel-wide progress becomes a child of the kernel slice it falls in. This is
   why trace processor merges them rather than the UI: the nesting is visible to
   queries (`parent_id`, `depth`), not only on screen.

Otherwise in-kernel events are ordinary slice, counter and flow rows. Location
tracks get GPU and domain dimensions from the GPU track descriptor support, so
queries can group by GPU and domain across processes. For example, the time each
worker spent waiting on dependencies (dimension names as proposed in that work):

```sql
SELECT
  extract_arg(t.dimension_arg_set_id, 'gpu') AS gpu,
  extract_arg(t.dimension_arg_set_id, 'gpu_domain') AS domain,
  SUM(s.dur) AS wait_ns
FROM slice s
JOIN track t ON s.track_id = t.id
WHERE s.name = 'wait deps'
GROUP BY gpu, domain;
```

Grouping by role instead compares one role across all workers, e.g. the time
each role spent waiting:

```sql
SELECT
  t.name AS role,
  COUNT(DISTINCT t.id) AS workers,
  SUM(s.dur) AS wait_ns
FROM slice s
JOIN track t ON s.track_id = t.id
WHERE s.name GLOB 'wait*'
GROUP BY role
ORDER BY wait_ns DESC;
```

Kernel-wide progress makes stragglers easy to find, e.g. how often each domain
was the last to complete a stage:

```sql
SELECT
  extract_arg(arg_set_id, 'debug.last_domain') AS domain,
  COUNT(*) AS stages
FROM slice
WHERE extract_arg(arg_set_id, 'debug.last_domain') IS NOT NULL
GROUP BY domain
ORDER BY stages DESC;
```

In the UI, a process's logical devices show their queues, with kernel-wide
progress merged in, and the location trees below. Queues bound this way are not
repeated in per-process GPU views. That placement is part of the GPU track
descriptor UI work; in-kernel events need nothing beyond it.

## Alternatives considered

### Option 1: Compose existing primitives in the logical domain (recommended)

Pro:

- No new event message, parser, table or UI code for the events themselves.
- Producers attach by identities they know (process, device, queue) and never
  need to know about other processes or other producers' track layouts.
- Neutral about vendor and recorder: the location vocabulary is the producer's.

Con:

- `TrackEvent` packets are larger than a packed fixed-size record.
- The same SM appears once per process; views across processes rely on
  dimensions rather than on a shared tree.

### Option 2: Machine-wide location trees under the GPU's hardware root

Put location trees and kernel-wide progress under the GPU's hardware root, next
to its hardware queues and counters.

Pro:

- One tree per SM across all processes, next to the hardware counters.

Con:

- Kernels of different processes interleave, or overlap, on the same tracks,
  recorded by producers that don't know about each other.
- Hardware queues have no identity another producer can name: `hw_queue_iid` is
  an interning ID scoped to one packet sequence, and trace processor keys
  hardware queue tracks on its value and the queue's name.

### Option 3: A dedicated in-kernel event message

A packed message with a location tuple (e.g. SM, warp), an event ID and a
timestamp, similar to how recorders lay out their device buffers.

Pro:

- Compact; maps 1:1 onto many recorders' buffers.

Con:

- Duplicates `TrackEvent` (nesting, flows, arguments) and needs its own parser,
  tables and UI.
- The location tuple bakes in one vendor's hierarchy.

### Option 4: Keep separate per-tool traces

Pro:

- No work in Perfetto.

Con:

- No alignment with the rest of the run and no merging across GPUs and
  processes, which is the problem this RFC addresses.

### Option 5: Per-launch track trees

Create a fresh set of location tracks for every launch.

Pro:

- Launch attribution falls out of the track structure.

Con:

- Track count grows with launches, and a location's activity across launches is
  split over many tracks. A per-launch lifetime slice on persistent tracks keeps
  launches apart without that cost.

### Option 6: Merge by name or merge key under re-declared parents

Each producer re-declares another producer's groupings (e.g. `CUDA ->
Device #0`) and relies on same-named siblings, or a shared `sibling_merge_key`,
to merge.

Pro:

- Uses existing merge behavior.

Con:

- Every producer has to know the other producers' names, groupings or keys,
  and the result depends on group tracks merging across files.

## Future work

**Machine-wide views.** Views of in-kernel data across all processes on a GPU,
and attaching to hardware queues, need identities beyond the process: in
particular a stable hardware queue ID scoped to `gpu_id`, which render-stage
events would carry (e.g. on `InternedGpuRenderStageSpecification`) and
`GpuTrackDescriptor` could declare, instead of identifying hardware queues by an
interning ID and a name.

**Linking in-kernel events to their launch.** In-kernel events should be
attributable to the individual launch they belong to, in the UI and in SQL.
Today a producer can name the per-launch lifetime slice after the kernel and
record the launch's `GpuRenderStageEvent.event_id` in a debug annotation, but
trace processor uses `event_id` only internally to resolve correlations, so the
annotation cannot be joined to the kernel slice. A typed link, e.g. an addition
to the `GpuCorrelation` track event extension, is left to a follow-up RFC.

## Open questions

1. **Placement of merged tracks.** When descriptors with the same identity sit
   under different parents, which one decides where the merged track goes: the
   one that carries render-stage events, the deepest one, or the first one
   seen?
2. **Two views of one render stage.** A render-stage event with both an
   `hw_queue_iid` and a `logical_queue_id` appears on the hardware queue and on
   the logical queue. Should trace processor store it as one slice shown on
   both tracks, or as one slice per track?
3. **Logical device identity.** Is the process and `gpu_id` enough when a
   process has several logical devices on one GPU (e.g. GPU partitions such as
   MIG instances, or several API contexts)? And should a producer that only
   knows the device UUID be able to name the logical device by it, instead of
   mapping it to the machine-wide `gpu_id` itself?
4. **Trace-scoped clocks.** Should a device clock be declarable once per trace
   (e.g. a global clock ID derived from a clock name, as reserved for IDs from
   128 up) instead of once per sequence?
5. **Cycle counters.** Is producer-side conversion enough, or should clocks
   support fractional unit multipliers or per-domain anchoring?
6. **Nested domains.** Is one `gpu_domain_id` per path enough for hardware with
   several block levels?
7. **Coverage metadata.** How should a producer record which locations it
   instrumented (e.g. 4 of 108 SMs) and the recorder's identity and overhead, so
   analyses can scale results?
8. **Level and role names.** Should we recommend names for well-known levels
   (SM, CU, PE, warp, wave) and common roles (e.g. loader, consumer, storer),
   or leave them to producers? Should the role become a track dimension, so a
   role-first view does not depend on track names?
9. **Slices that do not nest.** What should trace processor do with slices of a
   logical queue that do not nest with its render stages (e.g. a host-side
   annotation that ends while a kernel still runs, or kernels that overlap on
   one queue)? Drop them with a stat, or move them to an overflow lane as for
   other overlapping ranges?

## References

- K. Gupta, J. A. Stuart, J. D. Owens. "A Study of Persistent Threads Style GPU
  Programming for GPGPU Workloads". InPar 2012.
- NVIDIA CUTLASS. "Efficient GEMM in CUDA" (warp specialization and persistent
  kernel designs).
  <https://github.com/NVIDIA/cutlass/blob/main/media/docs/cpp/efficient_gemm.md>
- M. Osama, D. Merrill, C. Cecka, M. Garland, J. D. Owens. "Stream-K:
  Work-centric Parallel Decomposition for Dense Matrix-Matrix Multiplication on
  the GPU". PPoPP 2023. <https://arxiv.org/abs/2301.03598>
- M. Bauer, H. Cook, B. Khailany. "CudaDMA: Optimizing GPU Memory Bandwidth via
  Warp Specialization". SC 2011.
- J. Shah et al. "FlashAttention-3: Fast and Accurate Attention with Asynchrony
  and Low-precision". NeurIPS 2024. <https://arxiv.org/abs/2407.08608>
- "HipKittens: Fast and Furious AMD Kernels". 2025.
  <https://arxiv.org/abs/2511.08083>
- Hazy Research. "Look Ma, No Bubbles! Designing a Low-Latency Megakernel for
  Llama-1B". 2025. <https://hazyresearch.stanford.edu/blog/2025-05-27-no-bubbles>
- Hazy Research. Megakernels (per-SM, per-instruction timing and `.mkprof`
  viewer). <https://github.com/HazyResearch/Megakernels>
- FlashInfer profiler. <https://github.com/flashinfer-ai/flashinfer>
- NVIDIA CUTLASS. "In-Kernel Event Tracing" (CuTe DSL).
  <https://github.com/NVIDIA/cutlass/blob/main/media/docs/pythonDSL/guides/iket_profiling.rst>
- Cheng et al. "MPK: A Compiler and Runtime for Mega-Kernelizing Tensor
  Programs". OSDI 2026. <https://arxiv.org/abs/2512.22219>
- S. Laine, T. Karras, T. Aila. "Megakernels Considered Harmful: Wavefront Path
  Tracing on GPUs". HPG 2013.
