# Perfetto for Chromium developers

Perfetto is the default tracing system for Chromium. Record traces of desktop
Chrome from the Perfetto UI, record Chrome on Android together with a system
trace, and open traces saved by other tools. Chromium's own docs cover its trace
events in depth; these pages cover recording and analyzing the traces.

## What you can do

<div class="docs-home">
<div class="home-cards home-pair">
<section class="home-card"><h3><span class="home-icon material-icons-round role-record" aria-hidden="true">radio_button_checked</span>Record desktop Chrome</h3><p>Record every open tab from the Perfetto UI, choosing the trace categories you need.</p><ul><li><a href="/docs/getting-started/chrome-tracing">Record a Chrome trace</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-record" aria-hidden="true">smartphone</span>Record Chrome on Android</h3><p>Turn on the Chrome probe in an Android system trace to see Chrome next to the rest of the system.</p><ul><li><a href="/docs/getting-started/system-tracing">Record your first system trace</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-import" aria-hidden="true">upload_file</span>Open existing traces</h3><p>Open Chrome JSON traces, including exports from the DevTools Performance panel, and very large traces.</p><ul><li><a href="/docs/getting-started/other-formats">Supported trace and profile formats</a></li><li><a href="/docs/visualization/large-traces">Open large traces</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-engine" aria-hidden="true">table_chart</span>Query and annotate</h3><p>Query traces with PerfettoSQL and turn the results into debug tracks on the timeline.</p><ul><li><a href="/docs/analysis/perfetto-sql-getting-started">Query traces with PerfettoSQL</a></li><li><a href="/docs/analysis/debug-tracks">Turn query results into debug tracks</a></li></ul></section>
</div>
</div>

## Try it in five minutes

This records a trace of desktop Chrome from the browser. The first time, install
the
[Perfetto UI Chrome extension](https://chrome.google.com/webstore/detail/perfetto-ui/lfmkphfpdbjijhpomgecfikhfohaoine).

1.  Open [ui.perfetto.dev](https://ui.perfetto.dev) and click
    **Record new trace** in the sidebar.
2.  Set **Target platform** to **Chrome**.
3.  On the **Chrome browser** page, pick the categories you want to record.
4.  Click **Start tracing**, then do what you want to trace in the browser.
    Keep the Perfetto UI tab open: closing it stops tracing and loses the data.
5.  When recording stops, the trace opens in the UI.

WARNING: A Chrome trace contains the URLs and titles of all open tabs, extension
IDs and details about your hardware. Check before you share one.

For automated recording with crossbench, see
[Record a Chrome trace](/docs/getting-started/chrome-tracing.md).

## Next steps

### Analyze

- [Work with traces from the command line](/docs/getting-started/command-line-analysis.md): convert, query and summarize traces without opening the UI.
- [Analyze many traces at once](/docs/analysis/batch-trace-processor.md): run the same query over a set of traces.

### Understand

- [How trace analysis works](/docs/analysis/getting-started.md): how the Trace Processor turns a trace into tables you can query.

### Look up

- [Supported trace and profile formats](/docs/getting-started/other-formats.md): what the Chrome JSON importer supports.
- [PerfettoSQL syntax](/docs/analysis/perfetto-sql-syntax.md): the SQL dialect used in queries.
