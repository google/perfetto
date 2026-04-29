# Memory: ART Heap Dumps for Java/Kotlin Heap

NOTE: Capturing Heap dumps requires Android 11 or higher

NOTE: Perfetto ART Heap Dumps are distinct from standard JVM / HPROF heap
dumps. Unlike HPROF dumps, these only contain the reference graph - not the
data within objects.

See the [Memory Guide](/docs/case-studies/memory.md#java-hprof) for getting
started with ART (Android RunTime) heap dumps.

Conversely from [heap profiles](native-heap-profiler.md), heap dumps report
full retention graphs of Java objects but not call-stacks. The information
recorded in a heap dump is of the form: _Object X retains
object Y, which is N bytes large, through its class member named Z_.

Heap dumps are not to be confused with profiles taken by the
[Java Allocation Profiling](native-heap-profiler.md#java-heap-sampling), which
records allocation events / call stacks.

## UI

Heap dumps are shown as flamegraphs in the UI after clicking on the
diamond in the _"Heap Profile"_ track of a process. Each diamond corresponds to
a heap dump.

![Java heap dumps in the process tracks](/docs/images/profile-diamond.png)

![Flamegraph of a Java heap dump](/docs/images/java-heap-graph.png)

The native size of certain objects is represented as an extra child node in the
flamegraph, prefixed with "[native]". The extra node counts as an extra object.
This is available only on Android 13 or higher.

## SQL

Information about the Java Heap is written to the following tables:

* [`heap_graph_class`](/docs/analysis/sql-tables.autogen#heap_graph_class)
* [`heap_graph_object`](/docs/analysis/sql-tables.autogen#heap_graph_object)
* [`heap_graph_reference`](/docs/analysis/sql-tables.autogen#heap_graph_reference)

`native_size` (available only on Android T+) is extracted from the related
`libcore.util.NativeAllocationRegistry` and is not included in `self_size`.

For instance, to get the bytes used by class name, run the following query.
As-is this query will often return un-actionable information, as most of the
bytes in the Java heap end up being primitive arrays or strings.

```sql
select c.name, sum(o.self_size)
       from heap_graph_object o join heap_graph_class c on (o.type_id = c.id)
       where reachable = 1 group by 1 order by 2 desc;
```

|name                |sum(o.self_size)    |
|--------------------|--------------------|
|java.lang.String    |             2770504|
|long[]              |             1500048|
|int[]               |             1181164|
|java.lang.Object[]  |              624812|
|char[]              |              357720|
|byte[]              |              350423|

Using the standard library, we can query the normalize the graph into a tree,
always taking the shortest path to the root and get cumulative sizes.
From this we can see how much memory is being held by each type of object

```sql
INCLUDE PERFETTO MODULE android.memory.heap_graph.class_summary_tree;

SELECT
  -- The name of the class.
  name,
  -- The sum of `self_size` of this node and all descendants of this node.
  cumulative_size
FROM android_heap_graph_class_summary_tree;
```

| name | cumulative_size |
|------|-----------------|
|java.lang.String|1431688|
|java.lang.Class<android.icu.text.Transliterator>|1120227|
|android.icu.text.TransliteratorRegistry|1119600|
|com.android.systemui.statusbar.phone.StatusBarNotificationPresenter$2|1086209|
|com.android.systemui.statusbar.phone.StatusBarNotificationPresenter|1085593|
|java.util.Collections$SynchronizedMap|1063376|
|java.util.HashMap|1063292|

## Java thread call stacks

NOTE: Capturing Java thread call stacks alongside the heap graph requires
Android 16 (Baklava) or higher.

Each Java heap dump is paired with one call-stack sample per Java thread,
captured at the same instant the heap graph was snapshotted. The intent is
to answer "*who* was holding all this memory?" — the heap graph alone tells
you what is on the heap, the per-thread stacks tell you what code path
each thread was running when the heap was sampled.

The stack data lands in the standard perfetto stack-profiling tables, so
the existing call-tree / top-frame SQL recipes work as-is:

* [`stack_profile_frame`](/docs/analysis/sql-tables.autogen#stack_profile_frame) —
  one row per `(ArtMethod*, dex_pc)`. The `name` column is the method's
  `PrettyMethod()` (`"void android.os.Looper.loop()"`).
* [`stack_profile_callsite`](/docs/analysis/sql-tables.autogen#stack_profile_callsite) —
  parent-child chain of frames per call stack.
* [`perf_sample`](/docs/analysis/sql-tables.autogen#perf_sample) —
  one sample per Java thread per heap dump, joinable to `thread` /
  `process` via `utid` / `upid`. Threads with no Java frames (Signal
  Catcher, JIT pool, binder threads, etc.) appear with `callsite_id`
  NULL.

Each sample's `(ts, upid)` matches the heap dump's `(graph_sample_ts,
upid)` exactly, so a join lines them up cleanly:

```sql
SELECT
  hgo.graph_sample_ts AS dump_ts,
  thread.name AS thread_name,
  ps.callsite_id
FROM heap_graph_object hgo
JOIN perf_sample ps
  ON ps.ts = hgo.graph_sample_ts AND ps.upid = hgo.upid
LEFT JOIN thread USING(utid)
GROUP BY 1, 2, 3
ORDER BY 1, 2;
```

In `ui.perfetto.dev` the heap graph still appears as a diamond on the
"Heap Profile" track; the Java stack samples appear as additional
markers on each thread track at the same timestamp.

### Implementation note

The dumper runs in a forked child whose only kernel thread is the
perfetto producer — every other pthread is gone (POSIX semantics). The
parent's pre-fork `ScopedSuspendAll` snapshot, including each
`art::Thread*`'s `ManagedStack` / `top_quick_frame_` / `top_shadow_frame_`,
is preserved byte-for-byte in the child's address space. We walk those
in-memory fields with `art::StackVisitor` (`check_suspended=false`,
`kIncludeInlinedFrames`) — no kernel-state probes, no `/proc` reads —
to reconstruct each Java thread's stack. Source:
[`art/perfetto_hprof/perfetto_hprof.cc`](https://cs.android.com/android/platform/superproject/main/+/main:art/perfetto_hprof/perfetto_hprof.cc).

## TraceConfig

The Java heap dump data source is configured through the
[JavaHprofConfig](/docs/reference/trace-config-proto.autogen#JavaHprofConfig)
section of the trace config.

```protobuf
data_sources {
  config {
    name: "android.java_hprof"
    java_hprof_config {
      process_cmdline: "com.google.android.inputmethod.latin"
      dump_smaps: true
    }
  }
}
```
