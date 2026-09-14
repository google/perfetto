# Perfetto for Android developers

Perfetto is the default tracing system on Android. It records what your app,
the Android framework and the kernel are doing on one timeline, and profiles
where CPU time and memory go. Use it to find the cause of jank, slow startups,
ANRs and high memory use.

Android Studio's profiler and `ProfilingManager` record Perfetto traces too, so
you can open those files in [ui.perfetto.dev](https://ui.perfetto.dev) and use
everything on this page.

## What you can do

<div class="docs-home">
<div class="home-cards home-pair">
<section class="home-card"><h3><span class="home-icon material-icons-round role-record" aria-hidden="true">timeline</span>Record system traces</h3><p>See thread states, CPU scheduling, frame timelines and atrace slices for every process on the device.</p><ul><li><a href="/docs/getting-started/system-tracing">Record your first system trace</a></li><li><a href="/docs/how-to/choose-how-to-record">Choose how to record</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-record" aria-hidden="true">memory</span>Profile memory</h3><p>Find what allocates native memory with heapprofd, and what holds Java and Kotlin objects with ART heap dumps.</p><ul><li><a href="/docs/getting-started/memory-profiling">Profile native memory on Android</a></li><li><a href="/docs/how-to/art-heap-dump">Take an ART heap dump</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-record" aria-hidden="true">speed</span>Profile CPU</h3><p>Sample callstacks to see where CPU time goes in C++, Java and Kotlin code, next to scheduling data.</p><ul><li><a href="/docs/getting-started/cpu-profiling">Record CPU profiles and perf counters</a></li><li><a href="/docs/how-to/choose-a-profiler">Choose a profiler</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-engine" aria-hidden="true">table_chart</span>Query traces with SQL</h3><p>Ask questions of a trace with PerfettoSQL and plot the answers as tracks on the timeline.</p><ul><li><a href="/docs/getting-started/android-trace-analysis">Analyze Android traces with SQL</a></li><li><a href="/docs/analysis/debug-tracks">Turn query results into debug tracks</a></li></ul></section>
</div>
</div>

## Try it in five minutes

You need a device running Android 6 (Marshmallow) or later, connected over USB,
and `adb` on your `PATH`.

1.  Download the recording script:

    ```bash
    curl -O https://raw.githubusercontent.com/google/perfetto/main/tools/record_android_trace
    ```

2.  Record a 10 second trace of scheduling, CPU frequency and atrace
    annotations from every app:

    ```bash
    python3 record_android_trace -o trace_file.perfetto-trace -t 10s -b 32mb \
        -a '*' sched freq view ss input
    ```

    While it records, use your app on the device: open it, scroll and move
    between the screens you care about.

3.  When recording stops, the trace opens in the Perfetto UI. Find your app's
    process in the list of tracks and expand it to see its threads.

If you are working over SSH, add `--no-open` and open the file in
[ui.perfetto.dev](https://ui.perfetto.dev) yourself. To record from the browser
instead, see [Record your first system trace](/docs/getting-started/system-tracing.md).

## Next steps

### Solve a problem

- [Investigate memory use on Android](/docs/case-studies/memory.md): work out why an app uses more memory than expected.
- [Case study: a SystemUI scheduling blockage](/docs/case-studies/scheduling-blockages.md): follow a real investigation of a thread that could not run.

### Record

- [Record from the command line on Android](/docs/learning-more/android.md): use the on-device `perfetto` tool and full trace configs.
- [Instrument Android code with atrace](/docs/getting-started/atrace.md): add your own slices and counters to apps and platform code.
- [Record in the background](/docs/learning-more/tracing-in-background.md): start a long trace, disconnect, and collect the file later.
- [Record boot traces and heap dumps on OOM](/docs/getting-started/local-android-trace-recording.md): capture problems that happen at boot or when memory runs out.

### Understand

- [How Perfetto works](/docs/concepts/service-model.md): the tracing service, data sources and producers.
- [Trace configuration](/docs/concepts/config.md): how a trace config chooses buffers and data sources.

### Look up

- [Data sources](/docs/reference/data-sources.md): everything Perfetto can record, with the config for each.
- [FrameTimeline](/docs/data-sources/frametimeline.md): the data behind the expected and actual frame tracks.
- [Android version notes](/docs/reference/android-version-notes.md): which features are available on which Android release.
