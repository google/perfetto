# Memory: Java heap profiler

NOTE: The Java heap profiler requires Android 11 or higher

See the [Memory Guide](/docs/case-studies/memory.md#java-hprof) for getting
started with Java heap profiling.

Conversely from the [Native heap profiler](native-heap-profiler.md), the Java
heap profiler reports full retention graphs of managed objects but not
call-stacks. The information recorded by the Java heap profiler is of the form:
_Object X retains object Y, which is N bytes large, through its class member
named Z_.

## UI

Heap graph dumps are shown as flamegraphs in the UI after clicking on the
diamond in the _"Heap Profile"_ track of a process. Each diamond corresponds to
a heap dump.

![Java heap profiles in the process tracks](/docs/images/profile-diamond.png)

![Flamegraph of a Java heap profiler](/docs/images/java-flamegraph.png)

## SQL

Information about the Java Heap is written to the following tables:

* [`heap_graph_class`](/docs/analysis/sql-tables.autogen#heap_graph_class)
* [`heap_graph_object`](/docs/analysis/sql-tables.autogen#heap_graph_object)
* [`heap_graph_reference`](/docs/analysis/sql-tables.autogen#heap_graph_reference)

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
       from experimental_flamegraph(56785646801, 1, 'graph')
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

The Java heap profiler is configured through the
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
