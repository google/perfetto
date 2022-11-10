# Memory: Java heap dumps

NOTE: Capturing Java heap dumps requires Android 11 or higher

See the [Memory Guide](/docs/case-studies/memory.md#java-hprof) for getting
started with Java heap dumps.

Conversely from [Native heap profiles](native-heap-profiler.md), Java heap dumps
report full retention graphs of managed objects but not call-stacks. The
information recorded in a Java heap dump is of the form: _Object X retains
object Y, which is N bytes large, through its class member named Z_.

Java heap dumps are not to be confused with profiles taken by the
[Java heap sampler](native-heap-profiler.md#java-heap-sampling)

## UI

Heap graph dumps are shown as flamegraphs in the UI after clicking on the
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

We can use `experimental_flamegraph` to normalize the graph into a tree, always
taking the shortest path to the root and get cumulative sizes.
Note that this is **experimental** and the **API is subject to change**.
From this we can see how much memory is being held by each type of object

For that, we need to find the timestamp and upid of the graph.

```sql
select distinct graph_sample_ts, upid from heap_graph_object
```

graph_sample_ts     |        upid        |
--------------------|--------------------|
     56785646801    |         1          |

We can then use them to get the flamegraph data.

```sql
select name, cumulative_size
       from experimental_flamegraph
       where ts = 56785646801
            and upid = 1
            and profile_type = 'graph'
       order by 2 desc;
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
