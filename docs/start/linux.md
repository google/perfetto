# Perfetto for Linux developers

On Linux, Perfetto records kernel and system activity through ftrace, perf
events and procfs: scheduling, system calls, CPU frequency, function graphs and
callstack samples, on one timeline with your own tracepoints.

Everything you need to record ships in a single static binary, `tracebox`, that
you can copy to any machine.

## What you can do

<div class="docs-home">
<div class="home-cards home-pair">
<section class="home-card"><h3><span class="home-icon material-icons-round role-record" aria-hidden="true">timeline</span>Record system traces</h3><p>Capture scheduling, system calls, CPU frequency and process information from ftrace and procfs.</p><ul><li><a href="/docs/getting-started/system-tracing">Record your first system trace</a></li><li><a href="/docs/data-sources/cpu-scheduling">CPU scheduling</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-record" aria-hidden="true">speed</span>Profile native code</h3><p>Sample callstacks with perf events, symbolize them with your debug info, and profile heap allocations with heapprofd.</p><ul><li><a href="/docs/getting-started/linux-cookbook">Profile and trace native code on Linux</a></li><li><a href="/docs/learning-more/symbolization">Symbolize and deobfuscate profiles</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-record" aria-hidden="true">memory</span>Trace the kernel</h3><p>Add your own kernel tracepoints, follow kernel functions with function graph tracing, and keep ftrace data across a reboot.</p><ul><li><a href="/docs/getting-started/ftrace">Instrument the Linux kernel with ftrace</a></li><li><a href="/docs/data-sources/funcgraph">Kernel function graph tracing</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-engine" aria-hidden="true">table_chart</span>Query traces with SQL</h3><p>Find long system calls, blocked threads and busy CPUs with PerfettoSQL, in the UI or from scripts.</p><ul><li><a href="/docs/analysis/perfetto-sql-getting-started">Query traces with PerfettoSQL</a></li><li><a href="/docs/getting-started/command-line-analysis">Work with traces from the command line</a></li></ul></section>
</div>
</div>

## Try it in five minutes

You need a Linux machine and root access, because scheduling data comes from
ftrace.

1.  Download `tracebox`:

    ```bash
    curl -LO https://get.perfetto.dev/tracebox
    chmod +x tracebox
    ```

2.  Download a sample config that records scheduling data for 10 seconds:

    ```bash
    curl -LO https://raw.githubusercontent.com/google/perfetto/refs/heads/main/test/configs/scheduling.cfg
    ```

3.  Record the trace, and run the workload you care about while it records:

    ```bash
    sudo ./tracebox -o trace_file.perfetto-trace --txt -c scheduling.cfg
    ```

4.  Open `trace_file.perfetto-trace` in [ui.perfetto.dev](https://ui.perfetto.dev).
    Each CPU has its own track showing which thread ran when.

For more data sources and the reasoning behind each step, see
[Record your first system trace](/docs/getting-started/system-tracing.md).

## Next steps

### Record

- [Capture ftrace data across a reboot](/docs/data-sources/previous-boot-trace.md): dump the last seconds of ftrace data from the previous boot.
- [Trace multiple machines](/docs/learning-more/multi-machine-tracing.md): record events from two Linux machines into one trace.
- [Take periodic trace snapshots](/docs/getting-started/periodic-trace-snapshots.md): snapshot a continuous ring-buffer trace to monitor a machine over time.

### Understand

- [How Perfetto works](/docs/concepts/service-model.md): the tracing service, data sources and producers.
- [Buffers and data flow](/docs/concepts/buffers.md): how data moves from the kernel to the trace file, and how to size buffers.

### Look up

- [Data sources](/docs/reference/data-sources.md): everything Perfetto can record, with the config for each.
- [TRACEBOX(1)](/docs/reference/tracebox.md): every `tracebox` command and flag.
