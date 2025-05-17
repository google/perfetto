# Recording Memory Profiles with Perfetto

The memory use of a process plays a key but often non-obvious role in the
performance of processes. Understanding where and how your process is using
memory can give significant insight to understand why your process may be
running slower than you expect or just help make your program more efficient.

Perfetto uses two complementary techniques for understanding memory use: **heap
profiling** for native languages like C, C++ and Rust and **heap dumps** for
Java and other JVM languages which are garbage collected.

## Native (C/C++/Rust) Heap Profling

Native languages like C/C++/Rust commonly allocate and deallocate memmory at the
lowest level by using OS library functions called `malloc` and `free`. Native
heap profiling works by _intercepting_ calls to these functions and injecting
code which keeps track of the callstack where the memory was allocated and how
the

NOTE: native heap profiling with Perfetto only works on Android and Linux; this
is due to the techniques we use to intercept malloc and free only working on
these operating systems.

### Collecting your first heap profile

<?tabs>

TAB: Android

#### Prerequisites

* [ADB](https://developer.android.com/studio/command-line/adb) installed.
* A device running Android 10+.
* A _Profileable_ or _Debuggable_ app. If you are running on a _"user"_ build of
  Android (as opposed to _"userdebug"_ or _"eng"_), your app needs to be marked
  as profileable or debuggable in its manifest.
  See the [heapprofd documentation][hdocs] for more details.

[hdocs]: /docs/data-sources/native-heap-profiler.md#heapprofd-targets

##### Linux / macOS
Make sure adb is installed and in your PATH.

```bash
adb devices -l
```

If more than one device or emulator is reported you must select one upfront as follows:

```bash
export ANDROID_SERIAL=SER123456
```

Download the `tools/heap_profile` (if you don't have a perfetto checkout):

```bash
curl -LO https://raw.githubusercontent.com/google/perfetto/main/tools/heap_profile
chmod +x heap_profile
```

Then start the profile:

```bash
./heap_profile -n system_server
```

##### Windows

Make sure that the downloaded adb.exe is in the PATH.

```bash
set PATH=%PATH%;%USERPROFILE%\Downloads\platform-tools

adb devices -l
```

If more than one device or emulator is reported you must select one upfront as follows:

```bash
set ANDROID_SERIAL=SER123456
```

Download the
[heap_profile](https://raw.githubusercontent.com/google/perfetto/main/tools/heap_profile)
script. Then start the profile:

```bash
python /path/to/heap_profile -n system_server
```

TAB: Linux

TODO update below

Use heap_profile script

</tabs?>

### Visualizing your first heap profile

TODO update below

Upload the `raw-trace` file from the output directory to the
[Perfetto UI](https://ui.perfetto.dev) and click on diamond marker in the UI
track labeled _"Heap profile"_.

![Profile Diamond](/docs/images/profile-diamond.png)
![Native Flamegraph](/docs/images/native-heap-prof.png)

### Analysing your first heap profile

Video: run queries in UI

## Java/JVM Heap Dumps

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

## Next steps

Learn more about memory debugging in the
[Memory Usage on Android Guide](/docs/case-studies/memory.md) and more about the
[heapprofd data-source](/docs/data-sources/native-heap-profiler.md)
