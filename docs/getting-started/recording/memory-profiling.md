# Recording Memory Profiles with Perfetto

The memory use of a process plays a key role in the performance of processes and
impact on overall system statbility. Understanding where and how your process is
using memory can give significant insight to understand why your process may be
running slower than you expect or just help make your program more efficient.

When it comes to apps and memory, there are mainly two ways a process can use
memory:

- Native C/C++/Rust processes: typically allocate memory via libc's malloc/free
  (or wrappers on top of it like C++'s new/delete). Note that native allocations
  are still possible (and quite frequent) when using Java APIs that are backed
  by JNI counterparts. A canonical example is `java.util.regex.Pattern` which
  typically owns both managed memory on the Java heap and native memory due to
  the underlying use of native regex libraries.

- Java/Kt Apps: a good portion of the memory footprint of an
  app lives in the managed heap (in the case of Android, managed by ART's
  garbage collector). This is where evevery `new X()` object lives.

Perfetto offers two complimentary techniques for debugging the above:

- [**heap profiling**](#native-c-c-rust-heap-profling) for native code:
  this is based on sampling callstacks when a
  malloc/free happens and showing aggregated flamecharts to break down memory
  usage by call site.

- [**heap dumps**](#java-managed-heap-dumps) for Java/managed code:
  this is based on creating heap retention
  graphs that show retention dependencies between objects (but no call-sites).

## Native (C/C++/Rust) Heap Profling

Native languages like C/C++/Rust commonly allocate and deallocate memmory at the
lowest level by using the libc family of `malloc`/`free` functions. Native
heap profiling works by _intercepting_ calls to these functions and injecting
code which keeps track of the callstack of memory allocated but not freed.
This allows to keep track of the "code origin" of each allocation.
malloc/free can be perf-hotspots in heap-heavy processes: in order to mitigate
the overhead of the memory profiler we support
[sampling](/docs/design-docs/heapprofd-sampling) to trade-off accuracy and
overhead.

NOTE: native heap profiling with Perfetto only works on Android and Linux; this
is due to the techniques we use to intercept malloc and free only working on
these operating systems.

Heap profiling has a technical limitation: it is NOT retroactive.
Heap profiling is only able to observe and report
**allocations made after the trace started recording**. It is not able to
provide any insight for allocations that happened before recording the trace.
You can, of course, get a whole view by making sure to start trace recording
before even launching the app/process. You need to be able to reproduce the
memory leak/bloat.

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
* A _Profileable_ or _Debuggable_ app. If you are running on a _"user"_ build of
  Android (as opposed to _"userdebug"_ or _"eng"_), your app needs to be marked
  as profileable or debuggable in its manifest.
  See the [heapprofd documentation][hdocs] for more details.

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
* A _Profileable_ or _Debuggable_ app. If you are running on a _"user"_ build of
  Android (as opposed to _"userdebug"_ or _"eng"_), your app needs to be marked
  as profileable or debuggable in its manifest.
  See the [heapprofd documentation][hdocs] for more details.

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
[Perfetto UI](https://ui.perfetto.dev) and click on diamond marker in the UI
track labeled _"Heap profile"_.

![Profile Diamond](/docs/images/profile-diamond.png)
![Native Flamegraph](/docs/images/native-heap-prof.png)

The aggregated flamechart by default shows unreleased memory (i.e. memory that
has not been free()d) aggregated by call stack. The frames at the top represent
the earliest entrypoint in your call stack (typically `main()` or
`pthread_start()`). As you go towards the bottom, you'll get closer to the
frames that ultimately invoked `malloc()`.

You can also change the aggregation to the following modes:

![Heap Profiling modes](/docs/images/heapprof-modes.png)

* **Unreleased Malloc Size**: the default mode and aggregates callstacks by
  SUM(non-freed memory bytes).
* **Unreleased Malloc Count**: aggregates un-freed allocations by count,
  ignoring the size of each allocation. This can be useful to spot leaks of
  small size, where each object is small, but a large number of them accumulates
  over time.
* **Total Malloc Size**: aggregates callstack by bytes allocated via malloc(),
  whether they have been freed or not. This is helpful to investigate heap
  churn, code paths that create a lot of pressure on the allocator, even though
  they release memory in the end.
* **Total Malloc Count**: like the above, but aggregates by number of calls to
   `malloc()` and ignores the size of each allocation.

## Java/Managed Heap Dumps

NOTE: Java heap dumps with Perfetto only works on Android. This is due to the
deep integration with the JVM (Android Runtime - ART) required to effficiently
capture a heap dump without impacting the performance of the process.

### Collecting your first heap dump

TODO update below
Use java_heap_dump script

### Visualizing your first heap dump

TODO update below
Video: open trace in the UI

### Analysing your first heap dump

TODO update below
Video: run queries in UI


## Other types of memory

There are other, more subtle, ways a process can use memory:

- Directly invoking mmap() / passing tmpfs file descriptors around. We don't
  have a good solution to profile these.

- Native processes using custom allocators. This eventually decays in the mmap
  case above. However in this case we support instrumenting your custom
  allocator and offering heap profiling capabilities to it.
  See [/docs/instrumentation/heapprofd-api](heapprofd's Custom Allocator API)

- dmabuf: this typically happens with apps that exchange buffers directly with
  hardware blocks (e.g. Camera apps). We support tracking of dmabuf allocations
  in the timeline recorder [DOCUMENTATION NEEDED]

## Next steps

Learn more about memory debugging in the
[Memory Usage on Android Guide](/docs/case-studies/memory.md) and more about the
[heapprofd data-source](/docs/data-sources/native-heap-profiler.md)
