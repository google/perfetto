# Perfetto for performance engineers

Perfetto is a trace and profile viewer. Open CPU profiles from Linux perf,
pprof, simpleperf and samply, traces from Chrome, Firefox, macOS Instruments
and other tools, or your own data, and analyze all of it with SQL.

The UI runs locally in your browser and needs no installation.

## What you can do

<div class="docs-home">
<div class="home-cards home-pair">
<section class="home-card"><h3><span class="home-icon material-icons-round role-import" aria-hidden="true">local_fire_department</span>Open CPU profiles</h3><p>See perf, simpleperf and samply samples on a timeline, and pprof profiles as flamegraphs.</p><ul><li><a href="/docs/getting-started/viewing-cpu-profiles">Open a pprof, perf, simpleperf or samply profile</a></li><li><a href="/docs/how-to/choose-a-profiler">Choose a profiler</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-use" aria-hidden="true">account_tree</span>Explore flamegraphs</h3><p>Switch between flamegraph, call tree and functions views, top down or bottom up, and focus on the frames that matter.</p><ul><li><a href="/docs/getting-started/viewing-cpu-profiles#exploring">Explore the profile</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-import" aria-hidden="true">upload_file</span>Bring traces from other tools</h3><p>Open Chrome JSON, Firefox Profiler, Instruments, Fuchsia and ftrace text files, or convert your own data.</p><ul><li><a href="/docs/getting-started/other-formats">Supported trace and profile formats</a></li><li><a href="/docs/getting-started/converting">Convert your own data to a trace</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-engine" aria-hidden="true">table_chart</span>Analyze with SQL</h3><p>Query any of these with PerfettoSQL, from the UI, the command line or Python, and across many traces at once.</p><ul><li><a href="/docs/analysis/perfetto-sql-getting-started">Query traces with PerfettoSQL</a></li><li><a href="/docs/analysis/batch-trace-processor">Analyze many traces at once</a></li></ul></section>
</div>
</div>

## Try it in five minutes

Record a profile with the tool you already use, then open it in
[ui.perfetto.dev](https://ui.perfetto.dev).

<?tabs>

TAB: Linux perf

1.  Record a profile with callstacks:

    ```bash
    perf record -g -- ./my_program
    ```

2.  Convert it to text on the same machine, so `perf` can name the functions:

    ```bash
    perf script > profile.txt
    ```

3.  Open `profile.txt` in the UI. Samples appear on the timeline: select a time
    range to see its flamegraph.

TAB: pprof

1.  Record a CPU profile, for example from Go benchmarks:

    ```bash
    go test -bench . -cpuprofile cpu.pb.gz
    ```

2.  Open `cpu.pb.gz` in the UI. There is no need to decompress it. The profile
    opens as a flamegraph on the **Aggregate Profiles** page.

TAB: simpleperf

1.  Record your app with `app_profiler.py` from the `simpleperf/` directory of
    the Android NDK:

    ```bash
    python3 <NDK>/simpleperf/app_profiler.py \
        --app com.example.myapp \
        -r "-g --duration 10"
    ```

2.  Open the `perf.data` it writes in the UI. There is no need to convert it.

</tabs?>

For symbols, samply and memory profiles, see
[Open a pprof, perf, simpleperf or samply profile](/docs/getting-started/viewing-cpu-profiles.md).

## Next steps

### Analyze

- [Summarize traces into metrics](/docs/analysis/trace-summary.md): extract structured data from traces for comparison.
- [Analyze traces from Python](/docs/analysis/trace-processor-python.md): run PerfettoSQL from notebooks and scripts.
- [Merge traces in the UI](/docs/visualization/merging-traces.md): combine traces from several sources on one timeline.

### Understand

- [How trace analysis works](/docs/analysis/getting-started.md): how the Trace Processor turns a trace into tables you can query.

### Look up

- [Trace Processor command-line reference](/docs/reference/trace-processor-cli.md): every `trace_processor` command and flag.
- [PerfettoSQL syntax](/docs/analysis/perfetto-sql-syntax.md): the SQL dialect used in queries.
