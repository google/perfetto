# Profile native memory on Android

In this tutorial, you record a native heap profile of an Android app with
heapprofd, the Perfetto native heap profiler. At the end, you will have a trace
that shows which C/C++ call stacks allocated memory that was never freed. You
will look at it as a flamegraph in the Perfetto UI and query it with SQL.

This tutorial covers native memory only: `malloc`, `new` and everything built
on top of them. To look at Java/Kotlin objects instead, see
[Take an ART heap dump](/docs/how-to/art-heap-dump.md).

## Before you start

You need:

- A computer with [adb](https://developer.android.com/tools/adb) and Python 3
  installed.
- A device running Android 10 or later, connected over USB with USB debugging
  turned on.
- An app you can profile. On a _user_ build of Android (a normal consumer
  phone) the app must be debuggable or profileable. A debug build of your own
  app, installed from Android Studio, is debuggable. On _userdebug_ and _eng_
  builds any app works.

This tutorial uses `com.example.myapp` as the package name. Replace it with
yours.

## Step 1: Get the heap_profile script

If you have a Perfetto checkout, the script is at `tools/heap_profile`.
Otherwise, download it:

```bash
curl -LO https://raw.githubusercontent.com/google/perfetto/main/tools/heap_profile
```

Check that adb can see exactly one device:

```bash
adb devices -l
```

You should now see one line for your device. If you see more than one, pick
one with `export ANDROID_SERIAL=<serial>`.

## Step 2: Start the app

Open the app on the device, then check that its process is running:

```bash
adb shell pidof com.example.myapp
```

You should now see a process ID, such as `12345`. If the command prints
nothing, the app is not running or the name is wrong. The name must match the
NAME column of `adb shell ps -A`.

## Step 3: Record the profile

Start profiling:

```bash
python3 heap_profile android -n com.example.myapp
```

You should now see:

```text
Profiling active. Press Ctrl+C to terminate.
You may disconnect your device.
```

Use the app for about 30 seconds. Go through the screens or actions whose
memory use you care about. heapprofd only records allocations made while it
is running, so anything the app allocated before this step is not in the
profile.

Press Ctrl+C. The script stops the profiler, pulls the trace and converts it.
You should now see a line like this:

```text
Wrote profiles to /tmp/53dace (symlink /tmp/heap_profile-latest)
```

The directory depends on your system's temporary directory. It contains a
file named `raw-trace`: this is the trace you open in the next step.

TIP: You can record the same profile from the browser instead. Open the
[Record page](https://ui.perfetto.dev/#!/record), choose Android, open
**Memory**, turn on **Native heap profiling** and enter the process name.

## Step 4: Open the trace

1. Go to [ui.perfetto.dev](https://ui.perfetto.dev).
2. Click **Open trace file** in the sidebar and pick the `raw-trace` file from
   the output directory.

The UI selects the profile for you when the trace loads. You should now see
a process group for your app with a **Native heap profile** track, and a
**Native heap profile flamegraph** tab in the bottom panel.

Hover over the slice in the track. The tooltip shows three totals for the
profiling session: **Retained**, **Allocated** and **Delta**.

## Step 5: Read the flamegraph

![Native heap profile flamegraph](/docs/images/native-heap-prof.png)

The flamegraph starts in **Unreleased Malloc Size** mode. Each bar is a
function, and its width is the number of bytes allocated under that call stack
and not freed before the profile ended. Thread entry points such as
`__start_thread` sit at the top. Moving down takes you closer to the call that
reached `malloc`.

1. Click a wide bar near the bottom of the flamegraph. A popup shows its
   **Cumulative** and **Self** sizes and the library it lives in.
2. Open the metric dropdown on the left and pick **Total Malloc Size**. This
   counts every allocation, including memory that was later freed, so wide
   bars now point at allocation churn instead of retained memory.
3. Switch back to **Unreleased Malloc Size**.

You should now be able to name the two or three call stacks that hold the
most unreleased native memory in your app.

## Step 6: Query the profile with SQL

1. Click **Query (SQL)** in the sidebar.
2. Paste this query:

   ```sql
   INCLUDE PERFETTO MODULE android.memory.heap_profile.summary_tree;

   SELECT
     name,
     mapping_name,
     self_size,
     cumulative_size
   FROM android_heap_profile_summary_tree
   ORDER BY cumulative_size DESC
   LIMIT 20;
   ```

3. Click **Run Query**, or press Ctrl+Enter (Cmd+Enter on macOS).

`android_heap_profile_summary_tree` has one row per call stack node.
`cumulative_size` is the unreleased bytes for that frame and everything it
called; `self_size` counts only allocations where the frame is the leaf.

You should now see 20 rows. The first ones are thread entry points, and their
`cumulative_size` values line up with the widest bars at the top of the
flamegraph.

## What you just saw

heapprofd hooks `malloc` and `free` in the target process and records a call
stack for a sample of allocations, by default about one per 4 KiB allocated.
When the session ends it writes a snapshot of what was allocated and not
freed. The UI draws that snapshot as a slice on the process timeline and as a
flamegraph. The same data is available as SQL tables through the PerfettoSQL
standard library.

Because you targeted the app by name, heapprofd also profiles a new process
with that name from its startup, so you can restart the app during a session
to capture startup allocations.

## Next steps

- [Native heap profiler (heapprofd)](/docs/data-sources/native-heap-profiler.md):
  continuous dumps, sampling interval, target processes and troubleshooting.
- [heap_profile](/docs/reference/heap_profile-cli.md): every flag
  of the script.
- [Take an ART heap dump](/docs/how-to/art-heap-dump.md): see what is keeping
  Java/Kotlin objects alive.
- [Profile and trace native code on Linux](/docs/getting-started/linux-cookbook.md):
  the same workflow for a Linux binary.
- [Native heap profiler (heapprofd)](/docs/data-sources/native-heap-profiler.md):
  what the sampled numbers mean.
