# Take an ART heap dump

Capture a snapshot of the Java/Kotlin heap of an Android app and open it in
the Perfetto UI.

## Before you begin

- The device runs Android 11 or later. ART heap dumps are not available on
  older releases.
- On a _user_ build of Android, the app is debuggable or profileable. To make
  a release build profileable, add `<profileable android:shell="true"/>` to the
  `<application>` element of its manifest. On _userdebug_ and _eng_ builds,
  any app can be dumped.
- The app is running. A heap dump only targets processes that are already
  running when recording starts.
- You know the process name: the NAME column of `adb shell ps -A`. For an
  app's main process this is the package name, for example
  `com.example.myapp`.

## Take the dump

<?tabs>

TAB: Perfetto UI

1. Open the [Record page](https://ui.perfetto.dev/#!/record) and select
   **Android** as the target platform.
2. Under **Trace config**, choose **Empty**.
3. Select a transport and connect your device.
4. Open **Buffers and duration**. Set **In-memory buffer size** to 256 MB and
   **Max duration** to 30 s.
5. Open **Memory**, turn on **ART heap dumps** and enter the process name in
   the text box.
6. Click **Start tracing** and wait for the trace to finish.

TAB: Command line

1. Get `tools/java_heap_dump` from a Perfetto checkout, or download it:

   ```bash
   curl -LO https://raw.githubusercontent.com/google/perfetto/main/tools/java_heap_dump
   ```

2. Run it with the process name and an output file:

   ```bash
   python3 java_heap_dump -n com.example.myapp -o heap.pftrace
   ```

   The script waits for the dump to finish, then pulls the trace:

   ```text
   Dumping Java Heap.
   Wrote profile to heap.pftrace
   This can be viewed using https://ui.perfetto.dev.
   ```

3. Open [ui.perfetto.dev](https://ui.perfetto.dev), click **Open trace file**
   and pick `heap.pftrace`.

Useful flags: `-p` targets PIDs instead of names, `-b` sets the buffer size
(default `256mb`) and `-c <ms>` takes a dump every `<ms>` milliseconds until
you press Ctrl+C.

</tabs?>

## Check it worked

- A trace that contains only a heap dump opens straight into the
  **Heapdump Explorer**, on the **Overview** tab. It lists the reachable
  instance count and the bytes retained by each heap.
- If the trace also has timeline data, it opens on the timeline instead. Your
  app's process group has an **ART heap dump** track with one marker per dump,
  and the bottom panel shows the heap dump flamegraph with the **Object Size**
  metric. Click **Heapdump Explorer** in the sidebar to open the explorer.

If neither appears, see Troubleshooting below.

<details>
<summary>Deep dive: heap graph vs HPROF, and why a dump pauses the app</summary>

Perfetto asks ART to dump the heap by signaling the target process. ART then
records the heap graph: every reachable object, its class and size, the
references between objects and the GC roots. It does not record field values,
string contents or bitmap pixels. That makes the dump smaller and safe to
collect from production builds, but it cannot show what a string contains.

An HPROF dump from `adb shell am dumpheap` includes that content, and the
Heapdump Explorer can open it too. It requires a debuggable app and produces
much larger files.

Both kinds of dump need a consistent view of the heap, so the app's threads
stop while the snapshot is taken. For a Perfetto heap graph, ART forks the
process and walks the heap in the child, so the app itself resumes quickly.
The dump is still expensive: do not take one in the middle of an interaction
you are measuring.

</details>

## Troubleshooting

**The trace has no ART heap dump.**
The app is not profileable, so the profiler skipped it. Check
`adb logcat` for a `not profileable` message. Use a debuggable build, add the
`profileable` manifest tag, or use a userdebug device.

**The trace has no ART heap dump, and logcat shows nothing.**
The process name did not match, or the process was not running when recording
started. Compare the name with `adb shell ps -A`, start the app, then record
again.

**The UI says "The flamegraph is incomplete".**
The dump did not finish before the trace ended, or the buffer filled up. Raise
the buffer size (`-b 512mb`, or **In-memory buffer size** in the UI) and set a
longer **Max duration** on the Record page. Keep the app alive until recording
finishes.

**Class names look like `a.b.c`.**
The app is obfuscated with R8 or ProGuard. Deobfuscate the trace with your
mapping file; see
[Symbolize and deobfuscate profiles](/docs/learning-more/symbolization.md).

## Related

- [Heap Dump Explorer](/docs/visualization/heap-dump-explorer.md)
- [Record boot traces and heap dumps on OOM](/docs/getting-started/local-android-trace-recording.md)
- [ART heap dumps](/docs/data-sources/java-heap-profiler.md)
- [Investigate memory use on Android](/docs/case-studies/memory.md)
