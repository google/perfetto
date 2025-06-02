# Recording system traces with Perfetto

A powerful use of Perfetto is to collect tracing information from many different
processes and data sources on a single machine and combine them all into a
single trace. This allows debugging a wide range of performance and functional
problems including complex ones. Examples include problems which might span
multiple processes, between an app and the OS or even interactions between
hardware and the OS. Such traces are known as **system traces** or commonly
abbreviated to just **systraces**.

NOTE: Recording system traces with Perfetto is only supported out of the box on
**Android** and **Linux**. While trace recording daemons work on Windows and
macOS, there is no integration with system-level data sources meaning traces are
unlikely to be useful.

## Recording your first system trace

This section walks you through the process of recording your first system-wide
trace. There are multiple paths depending on whether you want to record on
Android with a GUI, on Android using the command line or on Linux (with the
command line only).

<?tabs>

TAB: Android (Perfetto UI)

**Prerequisites**

- Any Android device running R+ (if using an older version of Android, please
  see the _Android (command line)_ tab instead)
- A desktop/laptop with the Android device connected via a USB cable

**Instructions**

1. Start by navigating to [ui.perfetto.dev](https://ui.perfetto.dev). This is
   the **Perfetto UI**, our all-in-one graphical UI for recording, analysing and
   visualizing traces; we'll be making heavy use of this throughout the rest of
   the guide.
2. Click on "Record New Trace" on the left sidebar.
3. This should take you to the _Recording page_ of the UI which looks like this:
   ![Record page of the Perfetto UI](/docs/images/record-trace-adb-websocket-success.png)
4. You can choose between different ways of connecting to your Android device.
    Follow the instructions on the screen to connect to your device. Perfetto UI
    will check if all the condition is met or show a descriptive error message
    otherwise. For example, for the _ABD+Websocket_ transport the success
    message will look like on the screenshot above.

   NOTE: you may need to allow USB debugging on the device.

5. On the **Recording Settings** page, we can leave at in the default settings:

   - The **Recording Mode** option corresponds to the way in which the trace
     should be collected: "Stop when full" stops tracing when the tracing buffer
     is full, "Ring buffer" will overwrite the oldest data when the tracing
     buffer is full and "Long trace" will periodically flush the in-memory
     contents of the trace to a file allowing for multi-minute or even
     multi-hour long traces to be collected.
   - The **In-memory buffer size** option decides how much memory should be used
     on device to temporarily stage the contents of the trace before it is
     written to disk.
   - The **Max duration** option decides the maximum amount of the time the
     trace will continue for before stopping: you also can stop it any time
     manually using the "Stop Recording" button as you'll see later.

6. Now we can configure the exact types of tracing information we want to
   collect in the **Probes** sections. Feel free to explore the tabs and the
   options they contain: the UI should briefly explain what each option does and
   why it might be useful. For the purposes of this guide, we will want to
   enable the following probes:

   - In the **CPU** tab, enable the **Scheduling details** and **CPU frequency
     and idle states** options: this information allows us to understand what
     process/thread is running on each CPU over time and also what frequency the
     CPU was running at all times; this is very useful contextual information
     when investigating traces.
   - In the **Android Apps and Svcs** tab, enable the **Atrace Userspace
     annotations** and **Event log (logcat)** options. Under the Atrace
     userspace annotations, further enable the "System server", "View system"
     and "Input categories (press Ctrl/Cmd while clicking to perform
     multi-select).

     - Userspace Annotations provide context on what systems and apps are doing
       using tracing markers that developers have added through the
       `android.os.Trace` APIs.
     - Logcat adds `android.os.Log` messages into the trace file allowing
       analysing logging on the same timeline as other tracing data sources.

7. Click the green "Start Recording" button and, while the trace is recording,
   take some action on the Android device (e.g. opening an app, unlocking the
   phone etc).
8. After 10s, the trace will automatically stop and you will switch to the
   timeline view of the collected trace; this discussed in the next section.

TAB: Android (command line)

**Prerequisites**

- Any Android device running M+
- Desktop/Laptop with the Android device connected via a USB cable
- `adb` (Android Debug Bridge) executable to be available on your `PATH`
  - ADB binaries for Linux, Mac or Windows can be downloaded from
    https://developer.android.com/studio/releases/platform-tools

The Perfetto team provides a helper script for recording traces from the command
line on Android called `record_android_trace`. This takes care of much of the
heavy lifiting of collecting the trace, pulling it from the server and even
opening it in the Perfetto UI:

```bash
curl -O https://raw.githubusercontent.com/google/perfetto/main/tools/record_android_trace

# See python3 record_android_trace --help for more details.
python3 record_android_trace \
   -o trace_file.perfetto-trace
   # This option configure the trace to run for 10s.
   -t 10s \
   # This option configures the main buffer size to be 32MB.
   -b 32mb \
   # This option enables atrace annotations from all apps on the system.
   -a '*' \
   # These options specify some of the most important instrumentation on the
   # device: CPU scheduling/frequency to know what is running on the CPUs and
   # the importance the system placed on that work and atrace annotations to
   # have context on what was happening inside userspace processes (platform
   # and app).
   sched freq view ss input
```

The above command should cause a trace to be collected lasting 10 seconds: while
it runs, take some action on the Android device (e.g. opening an app, unlocking
the phone etc). When recording is complete, the trace will automatically be
opened in the Perfetto UI in a browser window.

NOTE: if you are running this on a remote machine (i.e. via SSH), you should
pass the flag `--no-open` (which prevents the automatic opening of the Perfetto
UI) and manually download the file at the printed path (e.g. with `scp`) and
open it in the UI; instructions on this are provided in the next section.

TAB: Linux (command line)

TODO Download tracebox. Collect a trace.

Perfetto can capture system traces on Linux. All ftrace-based data sources and
most other procfs / sysfs-based data sources are supported.

Currently there are no packages or prebuilts for Linux. In order to run Perfetto
on Linux you need to build it from source.

## Capturing a trace

Due to Perfetto's [service-based architecture](/docs/concepts/service-model.md),
in order to capture a trace, the `traced` (session daemon) and `traced_probes`
(probes and ftrace-interop daemon) need to be running. As per Perfetto v16, the
`tracebox` binary bundles together all the binaries you need in a single
executable (a bit like `toybox` or `busybox`).

#### Capturing a trace with ftrace and /proc pollers, no SDK

If you are interested in overall system tracing and are not interested in
testing the SDK, you can use `tracebox` in autostart mode as follows:

```bash
out/linux/tracebox -o trace_file.perfetto-trace --txt -c test/configs/scheduling.cfg
```

</tabs?>

## Viewing your first trace

We can now explore the captured trace visually by using the web-based trace
visualizer: the Perfetto UI.

NOTE: The Perfetto UI runs fully locally, in-browser using JavaScript +
WebAssembly. The trace file is **not** uploaded anywhere by default, unless you
explicitly click on the 'Share' link. The 'Share' link is available only to
Googlers.

The recording instructions above should all have caused the trace to
automatically open in the browser. However, if they did not work for any reasons
(most likely if you are running the commands over SSH), you can also open the
traces manually:

1. Navigate to [ui.perfetto.dev](https://ui.perfetto.dev) in a browser.
2. Click the **Open trace file** on the left-hand menu, and load the captured
   tracs.

![Perfetto UI with a trace loaded](/docs/images/system-tracing-trace-view.png)

- Explore the trace by zooming/panning using WASD, and mouse for expanding
  process tracks (rows) into their constituent thread tracks. Press "?" for
  further navigation controls.
- Please also take a look at our Perfetto UI
  [documentation page](/docs/visualization/perfetto-ui.md)  

## Querying your first trace

The trace you captured looks very complex, it could be hard to understand what
is going on. You can always open the **Query (SQL)** panel and write a Perfetto
SQL query. Perfetto SQL is a dialect of an SQL, see it
[syntax](/docs/analysis/perfetto-sql-syntax.md) and the rich
[standard library](/docs/analysis/stdlib-docs.autogen). 
In the screenshot below we can see the result of the following query:
```
INCLUDE PERFETTO MODULE android.garbage_collection;

select * from android_garbage_collection_events;
```

that returns the list of all Garbage Collection events, with additional
information for each event.

To further explore the trace and the standard library, you can separately
import each `MODULE` (no need to explicitly import `prelude`) and do the
`select * from` each table, e.g. the following query
```
select * from process;
```

returns the list of all processes captured by the trace.

Alternatively, you can explore the trace contents issuing SQL queries through
the [trace processor](/docs/analysis/trace-processor).

## Next steps

Learn more about recording:

The trace you captured consists of multiple **Data sources**, you can open the
interesting page from the left sidebar, some of them are listed here:
- [Heap profiler](/docs/data-sources/native-heap-profiler.md)
- [ATrace: Android system and app trace events](/docs/data-sources/atrace.md)

Learn more about trace analysis:

- PerfettoSQL [syntax](/docs/analysis/perfetto-sql-syntax.md) and the [standard library](/docs/analysis/stdlib-docs.autogen)
- Python [API](/docs/analysis/trace-processor-python.md) for programmatic trace analysis
- C++ [API](/docs/analysis/trace-processor.md) for programmatic trace analysis
