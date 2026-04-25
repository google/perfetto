# Frame jank

A frame is "janky" when the app fails to produce it before the
display's vsync deadline — the user sees a stutter. Perfetto's
**Actual Frame Timeline** track, per-process, colours every frame
green (on time), yellow (late but recoverable), red (deadline
missed) or blue (dropped). The headline screenshot of any jank
investigation is one of those red frames lined up vertically with
the main-thread slice that caused it. The
[FrameTimeline reference](/docs/data-sources/frametimeline.md)
covers the data source itself; this tutorial walks an end-to-end
investigation: capture, read, fix, verify.

This is the first tutorial in the
[Android performance tutorials](perf-tutorial-series.md) series.

NOTE: FrameTimeline data requires **Android 12 (API 31) or higher**.
On older devices the Expected/Actual tracks are absent — fall back
to reading `Choreographer#doFrame` and `RenderThread` slices on
the process timeline.

## Capture

Frame jank traces need three things: scheduling on every CPU,
[atrace](/docs/data-sources/atrace.md) events from the demo app's
`Trace.beginSection` markers, and the SurfaceFlinger
[frame timeline](/docs/data-sources/frametimeline.md). The full
config is in
[`artifacts/frame-jank/trace-configs/jank.cfg`](https://github.com/fiveapplesonthetable/perfetto/tree/perf-tutorials-artifacts/frame-jank/trace-configs/jank.cfg);
the relevant slice is:

```
data_sources: {
  config {
    name: "linux.ftrace"
    ftrace_config {
      ftrace_events: "sched/sched_switch"
      ftrace_events: "sched/sched_waking"
      atrace_categories: "gfx"
      atrace_categories: "view"
      atrace_categories: "sched"
      atrace_apps: "com.example.perfetto.jank"
    }
  }
}
data_sources: {
  config { name: "android.surfaceflinger.frametimeline" }
}
duration_ms: 12000
```

Run it against a debuggable build of the app while the scroll is
in progress (the demo's `JankActivity` starts auto-scrolling 1.5 s
after launch):

```bash
$ adb push trace-configs/jank.cfg /data/local/tmp/
$ adb shell am start -n com.example.perfetto.jank/.JankActivity
$ adb shell perfetto --txt -c /data/local/tmp/jank.cfg -o /data/local/tmp/before.pftrace
$ adb pull /data/local/tmp/before.pftrace
```

For an interactive recording UI (no config file needed) see
[Recording system traces](/docs/getting-started/system-tracing.md).

Drag `before.pftrace` onto [ui.perfetto.dev](https://ui.perfetto.dev).
The default view shows every CPU's scheduling rail at the top, and
the process tracks below:

![Perfetto UI with the buggy trace loaded. Top: per-CPU scheduling tracks. Below: process rows including `com.example.perfetto.jank`.](../images/frame-jank/01-trace-loaded.png)

## Case study: synchronous bitmap decode in `getView`

A developer reports that a `ListView` of image rows feels stuttery
on every scroll. The adapter is conventional — a `BaseAdapter` that
inflates a row, sets an `ImageView`, and returns it. The decode is
inline in the binding:

```java
@Override
public View getView(int position, View convertView, ViewGroup parent) {
    Trace.beginSection("BadAdapter.getView");
    try {
        // ... inflate row ...
        byte[] bytes = assets[position % assets.length];
        Bitmap bmp = BitmapFactory.decodeByteArray(bytes, 0, bytes.length); // <-- the bug
        ((ImageView) row.findViewById(1)).setImageBitmap(bmp);
        ((TextView) row.findViewById(2)).setText("Row " + position);
        return row;
    } finally {
        Trace.endSection();
    }
}
```

Each `getView` call decodes the asset from scratch on the UI thread.
The PNGs are 2048×2048, so each decode allocates 16 MiB and takes
tens of milliseconds — too long to fit inside a 16 ms vsync window.

### Find the jank

Search for the slice name (`/` opens search), then press `f` to zoom
to the selection. The Actual Frame Timeline track is the one to
read — green slices are healthy frames, red are missed deadlines:

![Perfetto UI zoomed onto a `BadAdapter.getView` slice. Expected Timeline (green) and Actual Timeline alternating red ("App Deadline Missed") and yellow show frames missing the vsync deadline because the main thread is busy decoding. Selected slice details in the bottom panel: BadAdapter.getView, duration 33 ms.](../images/frame-jank/02-buggy-jank.png)

The two tracks above the selection tell the story:

- **Expected Timeline** is what SurfaceFlinger asked for: one
  uniform-width green slice per vsync.
- **Actual Timeline** is what actually happened. Where Actual is
  red, the app missed its deadline. Where Actual is wider than
  Expected, the work spilled into the next frame.

The selected `BadAdapter.getView` slice in the bottom panel reports
**33 ms** — twice the 16 ms budget for a 60 Hz display. The pattern
holds across the trace: 274 of 275 `getView` calls take longer than
16 ms, average 33.87 ms. Every one of those binds is a missed
frame.

### Confirm with SQL (optional)

The frame timeline tables expose the same data the UI is rendering.
For the buggy trace:

```sql
SELECT jank_type, COUNT(*) AS n
FROM actual_frame_timeline_slice
WHERE upid = (SELECT upid FROM process WHERE name = 'com.example.perfetto.jank')
GROUP BY jank_type
ORDER BY n DESC;
```

returns `App Deadline Missed, Buffer Stuffing` on 222 frames out of
the ~12 s capture window. That number is the regression signal.

### Fix

Move the decode off the UI thread; cache the result so subsequent
binds are a map lookup, not a decode:

```java
private final LruCache<Integer, Bitmap> cache = new LruCache<>(8);
private final ExecutorService decoder = Executors.newSingleThreadExecutor();
private final Handler ui = new Handler(Looper.getMainLooper());

@Override
public View getView(int position, View convertView, ViewGroup parent) {
    // ... inflate row ...
    int key = position % assets.length;
    ImageView iv = row.findViewById(1);
    Bitmap cached = cache.get(key);
    if (cached != null) {
        iv.setImageBitmap(cached);
    } else {
        iv.setImageBitmap(null);
        byte[] bytes = assets[key];
        decoder.submit(() -> {
            Bitmap bmp = BitmapFactory.decodeByteArray(bytes, 0, bytes.length);
            ui.post(() -> { cache.put(key, bmp); iv.setImageBitmap(bmp); });
        });
    }
    return row;
}
```

The UI thread now does at most a `LruCache.get` and a
`setImageBitmap` per bind. Decodes happen on a background thread and
the result is cached.

### Verify

Recapture, re-load:

![Same zoom level on the fixed trace. Expected Timeline and Actual Timeline are uniformly green; the selected `GoodAdapter.getView` slice in the bottom panel is 27 µs — the bind is now cache-lookup-fast.](../images/frame-jank/03-fixed.png)

The Actual Timeline is now uniformly green. The selected
`GoodAdapter.getView` slice reports **27 µs** — three orders of
magnitude faster than the buggy version's 33 ms. The same SQL on the
fixed trace returns `App Deadline Missed`: 1 frame total, against
222 before — a 99.5% reduction.

`COUNT(*) FROM slice WHERE name = 'GoodAdapter.getView' AND dur > 16000000` is the
regression signal: any frame above 16 ms on the bind path is jank.

## When the UI thread is innocent

The Actual Frame Timeline can also turn red when the UI thread is
quiet. If the render thread blocks on a GPU-side upload (a large
`glBufferData`, an `eglSwapBuffers` stall, a synchronous fence
wait), the frame still misses its deadline, but the slice
responsible is on the **`RenderThread`** track for the same process,
not the main thread. Same Actual Frame Timeline reading, different
villain track.

The investigation pattern is identical:

1. Find a red frame in the Actual Frame Timeline. Note its time
   range.
2. Look at every track for the process at that range. The
   expensive slice is usually obvious.
3. If the main thread is idle and `RenderThread` is busy, the bug
   is on the GPU upload or composition path — typical fixes are
   batching uploads, uploading sub-images, or moving uploads off
   the render thread.
4. If both UI thread and `RenderThread` are quiet but the frame is
   still red, the bottleneck is downstream — SurfaceFlinger
   composition or display HAL. Switch to SurfaceFlinger's process
   tracks; its Expected/Actual timelines tell the same story for
   composition.

The colour reference and frame-id correlation between the app and
SurfaceFlinger's tracks live in the
[FrameTimeline data source doc](/docs/data-sources/frametimeline.md).

A worked second case study with a checked-in repro is on the
roadmap for this tutorial — when it lands it will live in
`artifacts/frame-jank/demo-buggy-renderthread/` and walk a
`Bitmap` upload that stalls the render thread without touching the
UI thread.

## See also

- [Android performance tutorials](perf-tutorial-series.md) — the
  series this tutorial is part of.
- [FrameTimeline data source](/docs/data-sources/frametimeline.md)
  — reference for the Expected/Actual tracks: colour meanings,
  jank-type taxonomy, SQL schema (`expected_frame_timeline_slice`,
  `actual_frame_timeline_slice`).
- [atrace](/docs/data-sources/atrace.md) — reference for the atrace
  categories used in the capture config (`gfx`, `view`, `sched`)
  and the per-app userspace markers (`Trace.beginSection`).
- [Heap Dump Explorer](/docs/visualization/heap-dump-explorer.md) —
  for memory leaks and retained-bitmap analysis (a common cause of
  graphics jank that doesn't show in the frame timeline).
- [Scheduling blockages](scheduling-blockages.md) — for jank caused
  by another process holding a lock or running at a higher
  priority on the same CPU.
