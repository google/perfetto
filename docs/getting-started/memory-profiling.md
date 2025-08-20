# Recording memory profiles with Perfetto

In this guide, you'll learn how to:

- Record native and Java heap profiles with Perfetto.
- Visualize and analyze heap profiles in the Perfetto UI.
- Understand the different memory profiling modes and when to use them.

The memory use of a process plays a key role in the performance of processes and
impact on overall system stability. Understanding where and how your process is
using memory can give significant insight to understand why your process may be
running slower than you expect or just help make your program more efficient.

When it comes to apps and memory, there are mainly two ways a process can use
memory:

- **Native C/C++/Rust processes**: typically allocate memory via libc's
  malloc/free (or wrappers on top of it like C++'s new/delete). Note that native
  allocations are still possible (and quite frequent) when using Java APIs that
  are backed by JNI counterparts. A canonical example is
  `java.util.regex.Pattern` which typically owns both **managed memory** on the
  Java heap and **native memory** due to the underlying use of native regex
  libraries.

- **Java/KT Apps**: a good portion of the memory footprint of an app lives in
  the **managed heap** (in the case of Android, managed by ART's garbage
  collector). This is where every `new X()` object lives.

Perfetto offers two complementary techniques for debugging the above:

- [**heap profiling**](#native-c-c-rust-heap-profling) for native code: this is
  based on sampling callstacks when a malloc/free happens and showing aggregated
  flamegraphs to break down memory usage by call site.

- [**heap dumps**](#java-managed-heap-dumps) for Java/managed code: this is
  based on creating heap retention graphs that show retention dependencies
  between objects (but no call-sites).

## Native (C/C++/Rust) Heap Profiling

Native languages like C/C++/Rust commonly allocate and deallocate memory at the
lowest level by using the libc family of `malloc`/`free` functions. Native heap
profiling works by _intercepting_ calls to these functions and injecting code
which keeps track of the callstack of memory allocated but not freed. This
allows to keep track of the "code origin" of each allocation. malloc/free can be
perf-hotspots in heap-heavy processes: in order to mitigate the overhead of the
memory profiler we support [sampling](/docs/design-docs/heapprofd-sampling) to
trade-off accuracy and overhead.

NOTE: native heap profiling with Perfetto only works on Android and Linux; this
is due to the techniques we use to intercept malloc and free only working on
these operating systems.

A very important point to note is that heap profiling is **not retroactive**. It
can only report allocations that happen _after_ tracing has started. It cannot
provide any insight into allocations that occurred before the trace began. If
you need to analyze memory usage from the start of a process, you must begin
tracing before the process is launched.

If your question is _"why is this process so big right now?"_ you cannot use
heap profiling to answer questions about what happened in the past. However our
anecdotal experience is that if you are chasing a memory leak, there is a good
chance that the leak will keep happening over time and hence you will be able to
see future increments.

### Collecting your first heap profile

<?tabs>

TAB: Android (Perfetto UI)

On Android Perfetto heap profiling hooks are seamlessly integrated into the libc
implementation.

#### Prerequisites

* A device running Android 10+.
* A [_Profileable_ or _Debuggable_](https://developer.android.com/topic/performance/benchmarking/macrobenchmark-instrumentation#profileable-apps)
  app. If you are running on a _"user"_ build of Android (as opposed to
  _"userdebug"_ or _"eng"_), your app needs to be marked as profileable or
  debuggable in its manifest. See the [heapprofd documentation][hdocs] for more
  details.

[hdocs]: /docs/data-sources/native-heap-profiler.md#heapprofd-targets

#### Instructions
- Open https://ui.perfetto.dev/#!/record
- Select Android as target device and use one of the available transports.
  If in doubt, WebUSB is the easiest choice.
- Click on the `Memory` probe on the left and then toggle the
  `Native Heap Profiling` option.
- Enter the process name in the `Names` box.
- The process name you have to enter is (the first argument of the) the process
  cmdline. That is the right-most column (NAME) of `adb shell ps -A`.
- Select an observation time in the `Buffers and duration` page. This will
  determine for how long the profile will intercept malloc/free calls.
- Press the red button to start recording the trace.
- While the trace is being recorded, interact with the process being profiled.
  Run your user journey, test patterns, interact with your app.

![UI Recording](/docs/images/heapprofd-ui.png)

TAB: Android (Command line)

On Android Perfetto heap profiling hooks are seamlessly integrated into the libc
implementation.

#### Prerequisites

* [ADB](https://developer.android.com/studio/command-line/adb) installed.
* _Windows users_: Make sure that the downloaded adb.exe is in the PATH.
  `set PATH=%PATH%;%USERPROFILE%\Downloads\platform-tools`
* A device running Android 10+.
* A [_Profileable_ or _Debuggable_](https://developer.android.com/topic/performance/benchmarking/macrobenchmark-instrumentation#profileable-apps)
  app. If you are running on a _"user"_ build of Android (as opposed to
  _"userdebug"_ or _"eng"_), your app needs to be marked as profileable or
  debuggable in its manifest. See the [heapprofd documentation][hdocs] for more
  details.

[hdocs]: /docs/data-sources/native-heap-profiler.md#heapprofd-targets

#### Instructions

```bash
$ adb devices -l
List of devices attached
24121FDH20006S         device usb:2-2.4.2 product:panther model:Pixel_7 device:panther transport_id:1
```

If more than one device or emulator is reported you must select one upfront as follows:

```bash
export ANDROID_SERIAL=24121FDH20006S
```

Download the `tools/heap_profile` (if you don't have a perfetto checkout):

```bash
curl -LO https://raw.githubusercontent.com/google/perfetto/main/tools/heap_profile
```

Then start the profile:

```bash
python3 heap_profile -n com.google.android.apps.nexuslauncher
```

Run your test patterns, interact with the process and press Ctrl-C when done
(or pass `-d 10000` for a time-limited profiling)

When you press Ctrl-C the heap_profile script will pull the traces and store
them in /tmp/heap_profile-latest. Look for the message that says

```bash
Wrote profiles to /tmp/53dace (symlink /tmp/heap_profile-latest)
The raw-trace file can be viewed using https://ui.perfetto.dev
```

TAB: Linux (Command line)

#### Prerequisites

* You need to build the `libheapprofd_glibc_preload.so` library from a Perfetto
  checkout ([instructions](/docs/data-sources/native-heap-profiler#-non-android-linux-support))

#### Instructions

Download tracebox and the heap_profile script
```bash
curl -LO https://get.perfetto.dev/tracebox
curl -LO https://raw.githubusercontent.com/google/perfetto/main/tools/heap_profile
chmod +x tracebox heap_profile
```

Start the tracing service
```bash
./tracebox traced &
```

Generate the heapprofd config and start the tracing session.
```bash
# Replace trace_processor_shell with with the name of the process you want to
# profile.
./heap_profile -n trace_processor_shell --print-config | \
  ./tracebox perfetto --txt -c - -o ~/trace_processor_memory.pftrace
```

Open another terminal (or tab), start the process you want to profile,
preloading the heapprofd's .so. Example:
```bash
PERFETTO_HEAPPROFD_BLOCKING_INIT=1 \
LD_PRELOAD=out/lnx/libheapprofd_glibc_preload.so \
trace_processor_shell /dev/null

# Typing this will cause a 40MB allocation.
> CREATE TABLE x as SELECT randomblob(40000000) as data
```

By default, heapprofd lazily initalizes to avoid blocking your program's
main thread. However, if your program makes memory allocations on startup,
these can be missed. To avoid this from happening, set the enironment variable
`PERFETTO_HEAPPROFD_BLOCKING_INIT=1`; on the first malloc, your program will
be blocked until heapprofd initializes fully but means every allocation will
be correctly tracked.

At this point:

* If your process terminates first, it will flush all the profiling data into
  the tracing buffer.
* If not, you can request a flush of the profiling data by stopping the trace.

In either case: press Ctrl-C on the `tracebox perfetto` command of the previous
step. That will write the trace file (e.g. `~/trace_processor_memory.pftrace`)
which you can then open with the Perfetto UI.

Remember also to kill the `tracebox traced` service once you are done.
```bash
fg
# Ctrl-C
```
</tabs?>

### Visualizing your first heap profile

Open the `/tmp/heap_profile-latest` file in the
[Perfetto UI](https://ui.perfetto.dev) and click on the chevron marker in the UI
track labeled _"Heap profile"_.

![Profile Diamond](/docs/images/profile-diamond.png)
![Native Flamegraph](/docs/images/native-heap-prof.png)

The aggregated flamegraph by default shows unreleased memory (i.e. memory that
has not been free()d) aggregated by call stack. The frames at the top represent
the earliest entrypoint in your call stack (typically `main()` or
`pthread_start()`). As you go towards the bottom, you'll get closer to the
frames that ultimately invoked `malloc()`.

You can also change the aggregation to the following modes:

![Heap Profiling modes](/docs/images/heapprof-modes.png)

- **Unreleased Malloc Size**: the default mode and aggregates callstacks by
  SUM(non-freed memory bytes).
- **Unreleased Malloc Count**: aggregates un-freed allocations by count,
  ignoring the size of each allocation. This can be useful to spot leaks of
  small size, where each object is small, but a large number of them accumulates
  over time.
- **Total Malloc Size**: aggregates callstack by bytes allocated via malloc(),
  whether they have been freed or not. This is helpful to investigate heap
  churn, code paths that create a lot of pressure on the allocator, even though
  they release memory in the end.
- **Total Malloc Count**: like the above, but aggregates by number of calls to
  `malloc()` and ignores the size of each allocation.

### Querying your first heap profile

As well as visualizing traces on a timeline, Perfetto has support for querying
traces using SQL. The easiest way to do this is using the query engine available
directly in the UI.

1.  In the Perfetto UI, click on the "Query (SQL)" tab in the left-hand menu.

    ![Perfetto UI Query SQL](/docs/images/perfetto-ui-query-sql.png)

2.  This will open a two-part window. You can write your PerfettoSQL query in
    the top section and view the results in the bottom section.

    ![Perfetto UI SQL Window](/docs/images/perfetto-ui-sql-window.png)

3.  You can then execute queries Ctrl/Cmd + Enter:

For example, by running:

```
INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_class_aggregation;

SELECT
  -- Class name (deobfuscated if available)
  type_name,
  -- Count of class instances
  obj_count,
  -- Size of class instances
  size_bytes,
  -- Native size of class instances
  native_size_bytes,
  -- Count of reachable class instances
  reachable_obj_count,
  -- Size of reachable class instances
  reachable_size_bytes,
  -- Native size of reachable class instances
  reachable_native_size_bytes
FROM android_heap_graph_class_aggregation;
```

you can see a summary of the reachable aggregate object sizes and object counts.

## Java/Managed Heap Dumps

Java—and managed languages built on top of it, like Kotlin—use a runtime
environment to handle memory management and garbage collection. In these
languages, (almost) every object is a heap allocation. Memory is managed through
object references: objects retain other objects, and memory is automatically
reclaimed by the garbage collector once objects become unreachable. There is no
free() call as in manual memory management.

As a result, most profiling tools for managed languages work by capturing and
analyzing a complete heap dump, which includes all live objects and their
retaining relationships—a full object graph.

This approach has the advantage of retroactive analysis: it provides a
consistent snapshot of the entire heap without requiring prior instrumentation.
However, it comes with a trade-off: while you can see which objects are keeping
others alive, you typically cannot see the exact call sites where those objects
were allocated. This can make it harder to reason about memory usage, especially
when the same type of object is allocated from multiple locations in the code.

NOTE: Java heap dumps with Perfetto only works on Android. This is due to the
deep integration with the JVM (Android Runtime - ART) required to efficiently
capture a heap dump without impacting the performance of the process.

### Collecting your first heap dump

<?tabs>

TAB: Android (Perfetto UI)

On Android Perfetto heap profiling hooks are seamlessly integrated into the libc
implementation.

#### Prerequisites

* A device running Android 10+.
* A [_Profileable_ or _Debuggable_](https://developer.android.com/topic/performance/benchmarking/macrobenchmark-instrumentation#profileable-apps)
  app. If you are running on a _"user"_ build of Android (as opposed to
  _"userdebug"_ or _"eng"_), your app needs to be marked as profileable or
  debuggable in its manifest.

#### Instructions
- Open https://ui.perfetto.dev/#!/record
- Select Android as target device and use one of the available transports.
  If in doubt, WebUSB is the easiest choice.
- Click on the `Memory` probe on the left and then toggle the
  `Java heap dumps` option.
- Enter the process name in the `Names` box.
- The process name you have to enter is (the first argument of the) the process
  cmdline. That is the right-most column (NAME) of `adb shell ps -A`.
- Select a short duration in the `Buffers and duration` page (10 s or less).
  The trace duration is meaningless for this particular data source, as it emits
  a whole dump at the end of the trace. A longer trace will not lead to more
  or better data.
- Press the red button to start recording the trace.

![UI Recording](/docs/images/jheapprof-ui.png)

TAB: Android (Command line)

On Android Perfetto heap profiling hooks are seamlessly integrated into the libc
implementation.

#### Prerequisites

* [ADB](https://developer.android.com/studio/command-line/adb) installed.
* _Windows users_: Make sure that the downloaded adb.exe is in the PATH.
  `set PATH=%PATH%;%USERPROFILE%\Downloads\platform-tools`
* A device running Android 10+.
* A [_Profileable_ or _Debuggable_](https://developer.android.com/topic/performance/benchmarking/macrobenchmark-instrumentation#profileable-apps)
  app. If you are running on a _"user"_ build of Android (as opposed to
  _"userdebug"_ or _"eng"_), your app needs to be marked as profileable or
  debuggable in its manifest.

#### Instructions

```bash
$ adb devices -l
List of devices attached
24121FDH20006S         device usb:2-2.4.2 product:panther model:Pixel_7 device:panther transport_id:1
```

If more than one device or emulator is reported you must select one upfront as follows:

```bash
export ANDROID_SERIAL=24121FDH20006S
```

Download the `tools/java_heap_dump` (if you don't have a perfetto checkout):

```bash
curl -LO https://raw.githubusercontent.com/google/perfetto/main/tools/java_heap_dump
```

Then start the profile:

```bash
python3 java_heap_dump -n com.google.android.apps.nexuslauncher
```
The script will record a trace with the heap dump and print the path of the
trace file (e.g. /tmp/tmpmhuvqmnqprofile)

```bash
Wrote profile to /tmp/tmpmhuvqmnqprofile
This can be viewed using https://ui.perfetto.dev.
```
</tabs?>

### Visualizing your first heap dump

Open the `/tmp/xxxx` file in the Perfetto UI and click on the chevron marker in
the UI track labeled "Heap profile".

The UI will show a flattened version of the heap graph, in the shape of a
flamegraph. The flamegraph aggregates together summing objects of the same type
that share the same reachability path. Two flattening strategies are possible:

- **Shortest path**: this is the default option when selecting `Object Size` in
  the flamegraph header. This arranges objects based on heuristics that minimize
  the distance between them.

- **Dominator tree**: when selecting `Dominated Size`, it uses the dominator
  tree algorithm to flatten the graph.

You can learn more about them in the
[Debugging memory usage](/docs/case-studies/memory#java-hprof) case study

![Sample heap dump in the UI](/docs/images/jheapprof-dump.png)

### Querying your first heap profile

As well as visualizing traces on a timeline, Perfetto has support for querying
traces using SQL. The easiest way to do this is using the query engine available
directly in the UI.

1.  In the Perfetto UI, click on the "Query (SQL)" tab in the left-hand menu.

    ![Perfetto UI Query SQL](/docs/images/perfetto-ui-query-sql.png)

2.  This will open a two-part window. You can write your PerfettoSQL query in
    the top section and view the results in the bottom section.

    ![Perfetto UI SQL Window](/docs/images/perfetto-ui-sql-window.png)

3.  You can then execute queries Ctrl/Cmd + Enter:

For example, by running:

```
INCLUDE PERFETTO MODULE android.memory.heap_profile.summary_tree;

SELECT
  -- The id of the callstack. A callstack in this context
  -- is a unique set of frames up to the root.
  id,
  -- The id of the parent callstack for this callstack.
  parent_id,
  -- The function name of the frame for this callstack.
  name,
  -- The name of the mapping containing the frame. This
  -- can be a native binary, library, JAR or APK.
  mapping_name,
  -- The name of the file containing the function.
  source_file,
  -- The line number in the file the function is located at.
  line_number,
  -- The amount of memory allocated and *not freed* with this
  -- function as the leaf frame.
  self_size,
  -- The amount of memory allocated and *not freed* with this
  -- function appearing anywhere on the callstack.
  cumulative_size,
  -- The amount of memory allocated with this function as the leaf
  -- frame. This may include memory which was later freed.
  self_alloc_size,
  -- The amount of memory allocated with this function appearing
  -- anywhere on the callstack. This may include memory which was
  -- later freed.
  cumulative_alloc_size
FROM android_heap_profile_summary_tree;
```

you can see the memory allocated by every unique callstack in the trace.

## Other types of memory

Besides the standard native and Java heaps, memory can be allocated in other
ways that are not profiled by default. Here are some common examples:

- **Direct `mmap()` calls**: Applications can directly request memory from the
  kernel using `mmap()`. This is often done for large allocations or to map
  files into memory. Perfetto does not currently have a way to automatically
  profile these allocations.

- **Custom allocators**: Some applications use their own memory allocators for
  performance reasons. These allocators often get their memory from the system
  using `mmap()` and then manage it internally. While Perfetto can't
  automatically profile these, you can instrument your custom allocator using
  the [heapprofd Custom Allocator API](/docs/instrumentation/heapprofd-api) to
  enable heap profiling.

- **DMA buffers (`dmabuf`)**: These are special buffers used for sharing memory
  between different hardware components (e.g., the CPU, GPU, and camera). This
  is common in graphics-intensive applications. You can track `dmabuf`
  allocations by enabling the `dmabuf_heap/dma_heap_stat` ftrace events in your
  trace configuration.

## Next steps

Now that you've recorded and analyzed your first memory profile, you can explore
more advanced topics:

- **Learn more about memory debugging:** The
  [Memory Usage on Android Guide](/docs/case-studies/memory.md) provides a deep
  dive into debugging memory issues on Android.
- **Explore the heapprofd data source:** The
  [heapprofd data source documentation](/docs/data-sources/native-heap-profiler.md)
  provides more details on the native heap profiler.
