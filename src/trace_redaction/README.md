# Trace Redaction

## Timeline

### Intro

The timeline is at the center of the redaction system. It provides an
efficient method to find which package a thread/process belongs to.

The timeline allows queries to be connected to time. Without this, there's a
significant privacy conern because a pid can be recycled. Just because the pid
is excluded from redaction before time T, doesn't mean it should be redacted
after time T.

### General Structure

The timeline uses an event-based pattern using two events:

- __Open Event:__ Marks the beginning of a pid's new lifespan.
- __Close Event:__ Marks the end of a pids's lifespan.

An event-based structure (compared to a span-based structure) is used as it is
better suited to handle errors/issues in the underlying data. For example, if a
pid doesn't explictly ends before being reused (e.g. two back-to-back open
events), the event-based structure "just works".

Open events contain the thread's full state. The close event only contains the
information needed to reference the thread's previous event.

```c++
struct Open {
    uint64_t ts;
    int32_t  pid;
    int32_t  ppid;
    uint64_t uid;
};

struct Close {
    uint64_t ts;
    int32_t  pid;
};
```

The vast majory of threads will have one event, an open event provided by the
`ProcessTree`. For some threads, they will have multiple open (`ProcessTree`,
`NewTask`) and close events (`ProcFree`) in alternating order.

### Query

```c++
struct Slice {
    int32_t  pid;
    uint64_t uid;
};

class Timeline {
  Slice Query(uint64_t ts, int32_t pid) const;
};

```

Events, regardless of type, are stored in contiguous memory and are ordered
first by pid and second by time. This is done to allow events to be found
via a binary search.

The vast majory of threads will have one event, the open event. Some threads
may have close and re-open events.

To handle a query,

1. Use a binary search to find the lower bound for `pid` (the first instance of
 `pid`)
1. Scan forward to find the last event before `ts` (for `pid`)

If an event was found:

```c++
if (e.type == kOpen && uid != 0)
  return Slice(pid, e.uid);

// The pid is active, check the parent for a uid.
if (e.type == kOpen && uid == 0)
  return Query(ts, e.ppid);

return Slice(pid, kNoPackage);
```

If `pid` does not have an immediate package (`uid`), the parent must be
searched. The parent-child tree is short, so the recursive search will be
relatively short. To minimize this even more, a union-find operation is applied
because any queries can be made.

__Simple runtime overview:__

Initialization:

- $sort + union\ find$

- $nlogn + mlogn$
  - where $n=events$
  - and $m=approx\ average\ depth$

Query:

- $P(p) = m_p * (logn + e_p)$
  - where $m_p=\ distance\ from\ pid\ to\ uid$
  - and $n=events$
  - and $e_p=number\ of\ events\ for\ process\ p$

- Because of the union-find in initialization, $m_p \to 0$

To further reduce the runtime, the search domain is reduces by remove all open
events for $pids$ that don't connect to a target $uid$. By removing open events,
and close events, there are two advantages:

1. Removing open events are safe and simple. By removing open events, those pids
can never be marked by active. Keeping the close events effectively reminds the
system that the pid is not active.

1. The number of open events exceeds the number of close events. Removing open
events will have a greater effect on the number of events.

__Example:__

|Name|Value|Notes|
|-|-|-|
|tids|3666|Total number of threads.|
|freed threads|5|Number of threads that were freed.|
|reused threads|0|No threads were used more than one time.|
|process tids|64|Total number of threads connected to the target process.|

After initialization, there would only be 64 open events and 5 close events.
This means that every uid lookup would be $logn\ |\ n=64 = 6$. Finding the uid
given a pid is one of the most common operations during redaction because uid
determines if something needs to be redacted.

## Scrub Task Rename Spec

### Background

`task_rename` are generated when a thread renames itself. This often happens
after (but not limited to) a `task_newtask` event. The `task_rename` event
exposes the threads old name and the threads new name.

### Protobuf Message(s)

__New task event:__

```javascript
event {
  timestamp: 6702094133317685
  pid: 6167
  task_newtask {
    pid: 7972
    comm: "adbd"
    clone_flags: 4001536
    oom_score_adj: -1000
  }
}
```

__Task rename event:__

```javascript
event {
  timestamp: 6702094133665498
  pid: 7972
  task_rename {
    pid: 7972
    oldcomm: "adbd"
    newcomm: "shell svc 7971"
    oom_score_adj: -1000
  }
}
```

### Method

A `task_rename` should be redacted when `event.pid` does not belong to that
target package (`context.package_uid`). Since the pid's naming information will
be removed everywhere, and naming information is effectively metadata, the whole
event can be dropped without effecting the integrity of the trace.

## Java Heap Dump Redaction

### Background

`trace_redactor <input> <output> <package> [--uid <target uid>]` handles Java
heap dump traces (ART `HeapGraph`, e.g. collected on OOM with a device-wide
`android.java_hprof.oom` config), system traces, and combined traces through a
single unified pipeline (`TraceRedactor::CreateInstance`). Heap dump traces
contain the package list, process tree and process stats of every app, and
possibly heap dumps of several apps; only the target package's data is kept.

### Trust model

Packets are classified by the `trusted_uid` stamped by traced:

- __System__ (`trusted_uid <= 9999`): only traced_probes (9999) is trusted for
  attribution (package uid, pid to uid).
- __App__ (`trusted_uid > 9999`): heap dumps are written by the app itself, and
  `app_packet_mask` restricts target-app packets to heap dump payloads only.
- __Missing uid__: e.g. a deobfuscation mapping appended after collection.

Heap dump primitives in Pass 1 (see `trace_redactor.cc`):

1. `FilterUntrustedAppPackets`: drop packets written by other apps.
2. `PruneMissingUidPackets` (configured with `AllowTargetDeobfuscationMapping`):
   drop uid-less packets unless an allowlisted `PacketFilter` accepts them.
3. `PruneHeapDumps`: keep `heap_graph` / `art_process_metadata` only from the
   target's uid and only for complete dumps (last packet on the sequence has
   `continued == false`). Non-target heap dumps (including from system-uid
   processes) and incomplete dumps are removed.
4. `BroadphasePacketFilter`: data-type allow-list (`packet_mask` for system
   packets, `app_packet_mask` for target-app packets).
5. `ScrubTrigger`, `ScrubServiceEvent`, `PrunePackageList`,
   `ScrubProcessStats`, `ReduceThreadsInProcessTrees`: remove other apps'
   rows and names.
6. `PruneShellPackets`: remove shell packets, i.e. packets left with only
   trusted uid/pid, sequence id and timestamp (+ clock id). Packets carrying
   incremental state, data loss markers or machine ids are kept.

Pass 2 is prepared dynamically after Pass 1 finishes: process-tree merging runs
only when Pass 1 observed more than one `ProcessTree` packet, and synthetic
sched merging runs only when Pass 1 observed ftrace CPUs. For heap dump traces
with no ftrace and at most one `ProcessTree`, Pass 2 stays empty and is skipped
with zero extra buffer allocation or copy.

### Timing

In heap dump traces the process stats (and the dump) are recorded before the
only process snapshot, and there is no ftrace. `CollectTimelineEvents`
back-dates the first open event of every pid to its
`process_start_from_boot` (or 0 when unknown and no ftrace is present) so
process stats can still be attributed. Heap dumps themselves are attributed by
`trusted_uid`, not by the timeline.

### Input requirements

The input must be uncompressed (decompress with `traceconv decompress_packets`
first). Truncated traces are rejected. A trace without a complete heap dump of
the target is not rejected: incomplete dumps are dropped, and the summary log
reports `complete_dumps_kept=0` plus a warning.
