#!/usr/bin/env python3
"""Regenerate the standalone catch-up report from the journal and test logs."""
from datetime import datetime, timezone
from html import escape
from pathlib import Path
import json
import re

ROOT = Path(__file__).resolve().parent
inventory = json.loads((ROOT / 'schema-inventory.json').read_text())
journal = (ROOT / 'journal.txt').read_text()


def result(log):
  path = ROOT / log
  if not path.exists():
    return 'Pending'
  text = path.read_text(errors='replace')
  matches = re.findall(r'\[  PASSED  \] (\d+) tests\.', text)
  failures = re.findall(r'\[  FAILED  \] (\d+) tests?', text)
  if failures:
    return f'{failures[-1]} failed — inspect log'
  if matches:
    return f'{matches[-1]} passed'
  return 'Running / inspect log'


validation = [
    ("New parser API and streaming replays", "tests-parser-api-focused.log",
     result("tests-parser-api-focused.log")),
    ("Unit suite after API migration", "tests-parser-api-all.log",
     result("tests-parser-api-all.log")),
    ("Parser diffs after API migration", "diff-parser-api.log",
     result("diff-parser-api.log")),
    ("Slice/args, flows and thread-counter replay",
     "tests-slice-args-focused.log", result("tests-slice-args-focused.log")),
    ("Current full TP unit suite", "tests-slice-all.log",
     result("tests-slice-all.log")),
    ("Current parser diffs", "diff-slice-final.log",
     result("diff-slice-final.log")),
    ("Thread-state checkpoint", "tests-thread-state-all.log",
     result("tests-thread-state-all.log")),
    ('Back-patches and independent frontiers', 'tests-backpatch-focused.log',
     result('tests-backpatch-focused.log')),
    ('Unit suite after back-patching', 'tests-backpatch-all.log',
     result('tests-backpatch-all.log')),
    ('Parser diffs after write-only annotation', 'diff-backpatch-parsers.log',
     result('diff-backpatch-parsers.log')),
    ('Frontier advancement and callback replay', 'tests-frontier-final.log',
     result('tests-frontier-final.log')),
    ('Unit suite after automatic advancement', 'tests-frontier-all.log',
     result('tests-frontier-all.log')),
    ('Streaming storage, frontier and parser replays',
     'tests-streaming-final.log', result('tests-streaming-final.log')),
    ('Earlier unit checkpoint', 'tests-final-tp.log',
     result('tests-final-tp.log')),
    ('Earlier parser checkpoint', 'diff-final-parsers.log',
     result('diff-final-parsers.log')),
    ('Earlier TP checkpoint (replay tests excluded)', 'tests-existing-tp.log',
     result('tests-existing-tp.log')),
    ('Earlier JSON checkpoint', 'diff-json.log', result('diff-json.log')),
    ('Earlier core-parser checkpoint', 'diff-core-parsers.log',
     result('diff-core-parsers.log')),
]
benchmark_path = ROOT / 'benchmark-summary.json'
performance = '<p>Measurements pending.</p>'
if benchmark_path.exists():
  benchmark = json.loads(benchmark_path.read_text())
  performance = '<p>' + escape(benchmark['methodology']) + '</p>'
  performance += '<table><thead><tr><th>Workload</th><th>Mode</th><th>CPU seconds</th><th>Peak RSS MiB</th><th>Storage frontier</th><th>Completion frontier</th></tr></thead><tbody>'
  for row in benchmark['results']:
    performance += f"<tr><td>{row['workload']}</td><td>{row['mode']}</td><td>{row['cpu_s']:.3f}</td><td>{row['peak_rss_mib']:.1f}</td><td>{row['sched_frontier']}</td><td>{row.get('completion_frontier', 0)}</td></tr>"
  performance += '</tbody></table><p>' + escape(benchmark['baseline']) + '</p>'
  performance += '<p><a href="benchmark-summary.json">Summary</a> · <a href="benchmark-runs.json">All runs</a> · <a href="benchmark.cc">Harness</a> · <a href="run-benchmarks.py">Runner</a> · <a href="generate-benchmark-input.py">Input generator</a></p>'

slice_performance = '<p>Slice measurements pending.</p>'
if (ROOT / 'benchmark-slices-summary.json').exists():
  data = json.loads((ROOT / 'benchmark-slices-summary.json').read_text())
  slice_performance = '<h3>One million child slices under one long-lived parent</h3><p>TrackEvent input with two begin args and one end arg per child. Repeated: 64 distinct child arg sets; unique: three million args rows. Five fresh-process medians, same SQL-free binary and checksum sink as above.</p>'
  slice_performance += '<table><thead><tr><th>Args</th><th>Mode</th><th>CPU seconds</th><th>Peak RSS MiB</th><th>Slice frontier</th><th>Args frontier</th></tr></thead><tbody>'
  for row in data['results']:
    slice_performance += f"<tr><td>{row['workload']}</td><td>{row['mode']}</td><td>{row['cpu_s']:.3f}</td><td>{row['peak_rss_mib']:.1f}</td><td>{row['slice_frontier']}</td><td>{row['arg_frontier']}</td></tr>"
  slice_performance += '</tbody></table><p>Slice mode emits all slice and args cells without storing them. Slice storage advances past the still-open parent; args completion advances after complete immutable arg sets. Checksums match across sink/drop/slice. Unique-args RSS includes the unchanged global deduplication map and is not bounded.</p><p><a href="benchmark-slices-summary.json">Slice summary</a> · <a href="benchmark-slices-runs.json">All slice runs</a> · <a href="run-slice-benchmarks.py">Runner</a> · <a href="generate-slice-input.py">Generator</a></p>'

scaling = ''
for filename, title in [
    ('benchmark-scaling-summary.json', 'Scheduling scaling'),
    ('benchmark-slices-scaling-summary.json', 'Slice scaling')
]:
  path = ROOT / filename
  if path.exists():
    scaling += '<details><summary>' + title + '</summary><pre>' + escape(
        path.read_text(
        )) + '</pre><a href="' + filename + '">Raw summary</a></details>'

rows = ''.join('<tr><td>' + escape(name) + '</td><td>' + escape(status) +
               '</td><td><a href="' + log + '">Log</a></td></tr>'
               for name, log, status in validation)
source_files = [
    ('Public TraceParser entrypoint',
     '../include/perfetto/trace_processor/trace_parser.h'),
    ('Parser-only configuration',
     '../include/perfetto/trace_processor/trace_parser_config.h'),
    ('Parser lifecycle and importer registration',
     '../src/trace_processor/trace_parser_impl.cc'),
    ('Slice working state and late patches',
     '../src/trace_processor/importers/common/slice_tracker.cc'),
    ('Cached flow source timestamps',
     '../src/trace_processor/importers/common/flow_tracker.cc'),
    ('Immutable args emission and exact deduplication',
     '../src/trace_processor/importers/common/global_args_tracker.h'),
    ('Cached thread-state working state',
     '../src/trace_processor/importers/common/thread_state_tracker.cc'),
    ('Config and early installation hook',
     '../include/perfetto/trace_processor/basic_types.h'),
    ('Generated table sinks and retention masks',
     '../python/generators/trace_processor_table/serialize.py'),
    ('Dataframe inserts, updates, omission and frontier',
     '../src/trace_processor/core/dataframe/dataframe.h'),
    ('Physical compaction and query checks',
     '../src/trace_processor/core/dataframe/dataframe.cc'),
    ('Ftrace scheduling frontier advancement',
     '../src/trace_processor/importers/ftrace/ftrace_sched_event_tracker.cc'),
    ('Core/plugin sink factory',
     '../src/trace_processor/storage/trace_storage.cc'),
    ('SQL initialization and EOF adjustments',
     '../src/trace_processor/trace_processor_impl.cc'),
    ('Parser replay tests',
     '../src/trace_processor/streaming_tables_unittest.cc'),
    ('Storage and frontier tests',
     '../src/trace_processor/core/dataframe/dataframe_unittest.cc'),
]
links = ''.join('<li><a href="' + path + '">' + escape(name) + '</a></li>'
                for name, path in source_files)
html = '''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Perfetto parser streaming — morning catch-up</title>
<style>
:root{color-scheme:light;--ink:#192a3c;--muted:#596b80;--line:#dce4ee;--accent:#1858a8}
*{box-sizing:border-box}body{margin:0;background:#f5f7fb;color:var(--ink);font:16px/1.65 system-ui,sans-serif}
main{max-width:1120px;margin:40px auto;padding:0 24px 60px}h1{font-size:36px;line-height:1.2;margin-bottom:12px}h2{font-size:24px;margin-top:0}
nav{display:flex;flex-wrap:wrap;gap:18px;margin:24px 0}a{color:var(--accent)}section{background:white;border:1px solid var(--line);border-radius:14px;padding:26px;margin:22px 0}
.cards{display:grid;grid-template-columns:repeat(3,1fr);gap:14px}.card{background:#edf3fb;padding:18px;border-radius:10px}.card strong{display:block;font-size:27px}
.callout{border-left:4px solid #bd770b;background:#fff7e8;padding:14px 18px}.good{border-left:4px solid #278358;background:#edf8f1;padding:14px 18px}
table{border-collapse:collapse;width:100%;font-size:14px}th,td{border-bottom:1px solid var(--line);padding:10px;text-align:left;vertical-align:top}th{background:#f0f4fa}
pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#f2f5fa;padding:18px;border-radius:8px;font:13px/1.6 ui-monospace,monospace}code{font:14px ui-monospace,monospace}small,.muted{color:var(--muted)}details{margin:14px 0}summary{cursor:pointer;font-weight:600}
input[type=search]{width:100%;padding:12px;border:1px solid var(--line);border-radius:8px;margin-bottom:16px}.scroll{overflow-x:auto}.schema{max-height:450px;overflow:auto}
#frontier-rows{display:flex;gap:4px;flex-wrap:wrap;margin:18px 0}.row{width:38px;padding:7px 0;text-align:center;border-radius:5px;background:#dcecfb}.row.gone{background:#e7e9ed;color:#919aa4;text-decoration:line-through}.row.pinned{background:#f8d89d;border:2px solid #b66c00;padding:5px 0}
@media(max-width:700px){.cards{grid-template-columns:1fr}h1{font-size:28px}section{padding:18px}main{padding:0 14px}}
</style></head><body><main>
<h1>Parser streaming: morning catch-up</h1>
<p>Implemented a general insert/update sink boundary, conservative column omission, and automatic frontier advancement for scheduling, thread-state, slices and args. The source remains in the shared workspace; no commit or PR was created.</p>
<p class="muted">Report generated GENERATED · Starting checkout: <code>4a4addaf02</code> · <a href="https://github.com/google/perfetto/pull/7719#issuecomment-5927793394">Original proposal</a></p>
<nav><a href="trace-parser-api.html">TraceParser API</a><a href="#outcome">Outcome</a><a href="#decisions">Decisions</a><a href="#frontier">Frontier prototype</a><a href="#performance">CPU/RSS</a><a href="#validation">Validation</a><a href="#inventory">Column audit</a><a href="#journal">Journal</a></nav>
<section id="outcome"><h2>What is implemented</h2>
<div class="cards"><div class="card"><strong>115</strong>core table schemas audited</div><div class="card"><strong>338 / 741</strong>explicit core columns eligible for omission</div><div class="card"><strong>8 KiB</strong>asserted allocation for a 1,024-row int64 window</div></div>
<ul><li>Every generated table has a nested <code>Sink</code> interface with typed <code>OnRowInsert(Id, Row)</code> and <code>OnCellUpdate(Id, column, value)</code> callbacks.</li>
<li>Dataframe-level transport also catches cursor setters and generic args writes. Omitted columns keep their value and null storage empty; IDs and total row counts still advance.</li>
<li>The existing layouts remain in use for retained columns. Without streaming configured, ordinary insertion uses the existing storage path.</li>
<li>A construction-time hook installs sinks before startup metadata is inserted. The factory also configures plugin-owned registered tables.</li>
<li><code>DropRowsBefore(frontier)</code> physically compacts storage and preserves absolute IDs. Evicted rows still accept sink-only patches. A separate completion frontier promises no more updates.</li><li>LOW_PERF_WRITE/HIGH_PERF_WRITE annotations expose setters without getters. Such columns have no local storage in omission mode; sched.end_state is the first audited conversion.</li><li>Ftrace scheduling keeps open timestamps in CPU state and stores no sched cells in experimental mode.</li></ul>
<p class="good"><strong>Original slice/args motivation now prototyped:</strong> JSON and TrackEvent can emit slice/args cells without retaining historical rows. Parser reads use active slice and flow state instead. Long-lived parents receive sink-only patches after eviction.</p><p>The conservative 338-column count is separate from these audited per-table policies and is not an RSS-saving estimate.</p>
<p>This is a working architectural prototype. The performance probe constructs no query engine. A distinct TraceParser API and SQL-free dependency target are now implemented. Full parser/working-table separation and trace-wide retention policies remain unfinished.</p></section>
<section id="decisions"><h2>Major decisions and workarounds</h2><div class="scroll"><table><thead><tr><th>Decision</th><th>Reason and effect</th></tr></thead><tbody>
<tr><td>Insert/update contract</td><td>Consumers receive provisional inserts plus subsequent cell updates. Immutable-file or aggregation consumers must resolve patches themselves; completion is explicitly signalled by OnFrontierAdvance; storage eviction alone does not finalize records.</td></tr>
<tr><td>Shared Dataframe plumbing</td><td>TraceStorage placement-constructs Dataframes and treats them as generated tables. Keeping sink state in Dataframe preserves that exact layout relationship.</td></tr>
<tr><td>Retain query-only exceptions</td><td>HeapGraphObjectTable.upid and HeapGraphReferenceTable.reference_set_id were NONE but are used by parser query filters. They are now annotated READ.</td></tr>
<tr><td>Configure before first insert</td><td>A Config hook runs when TraceStorage is constructed, before trackers insert metadata. Reconfiguration after rows exist is rejected.</td></tr>
<tr><td>Configure plugin tables at registration</td><td>Some importer tables live outside TraceStorage. Generated tables carry their mask; runtime schemas retain all columns because omission cannot be proven.</td></tr>
<tr><td>Skip standard SQL setup in omission mode</td><td>The SQL prelude prepares views over omitted columns. Omission mode skips the prelude, SQL metric registration and SQL bounds population. That was the initial workaround. Retention options now live in private TraceParserOptions, and the new TraceParser target creates no SQL connection; TraceProcessor always retains its standard SQL setup.</td></tr>
<tr><td>Skip indexes over omitted columns</td><td>EOF finalization would otherwise attempt to scan empty arrays. Default-mode indexing is unchanged.</td></tr>
<tr><td>Reject invalid reads and reentrant mutations</td><td>Declared projection/filter/sort/distinct access to dropped columns is rejected. Typed reads of dropped or evicted data fail explicitly.</td></tr>
<tr><td>Separate eviction from completion</td><td>DropRowsBefore emits OnStorageFrontierAdvance and forbids earlier reads. AdvanceCompletionFrontier emits OnFrontierAdvance and forbids earlier writes. Typed and generic setters deliver back-patches without recreating evicted data.</td></tr><tr><td>Save open CPU timestamps</td><td>Ordinary and compact ftrace closes calculate duration from CPU pending state, rather than reading sched.ts. In experimental ftrace mode every sched cell is sink-only. Generic-kernel and other scheduling producers retain their existing fallback.</td></tr><tr><td>Batched physical prefix compaction</td><td>The storage has no prefix-drop primitive. The prototype copies the retained tail and returns excess allocation capacity. Frequent tiny drops can be expensive; chunked storage is a future optimization.</td></tr>
<tr><td>Slice and flow working state</td><td>Keep active slice timestamps, duration, matching keys and thread counters in the stack; cache flow source timestamps. Save the last popped slice for immediate end-event patches. Default getters retain table behavior for other importers.</td></tr><tr><td>Exact args deduplication retained</td><td>Immutable args rows can be evicted and finalized immediately. The hash-to-ID cache is preserved, so previously emitted arg-set IDs are reused exactly. Its memory scales with distinct argument sets.</td></tr><tr><td>Conservative slice completion</td><td>Slice storage advances every 1024 rows but completion stays zero: deferred translations and end-event updates remain valid. Active parents do not pin local storage.</td></tr><tr><td>Build without ccache</td><td>The cache directory is outside writable roots. CCACHE_DISABLE=1 avoids that environment restriction.</td></tr>
<tr><td>Journal outside .codex</td><td>.codex is read-only in this workspace. All catch-up artifacts are in streaming-parser-audit.</td></tr>
</tbody></table></div></section>
<section id="frontier"><h2>What “keep the last N rows” becomes</h2>
<p>A table's total row count and IDs remain absolute. Its physical arrays represent only rows at or after <code>first_retained_row</code>. Typed access translates an absolute row into a physical position.</p>
<pre>logical ID 130000 → physical slot 130000 - first_retained_row
DropRowsBefore(130048) → retain IDs [130048, 131072)
row_count() remains 131072; the next inserted ID is 131072</pre>
<p>The test feeds 131,072 rows, advances the frontier in batches, and checks that the final 1,024 int64 values occupy exactly 8,192 allocated bytes. It also tests sparse nulls across word boundaries, late null/value updates, dropping all retained rows, and appending afterward.</p>
<p><strong>Storage can now advance past open rows.</strong> Each CPU stores its pending start timestamp outside the table. In experimental ftrace mode all sched cells are sink-only, and storage advances to the cumulative row count every 1,024 inserts. No sched value/null arrays are allocated, including for an open slice.</p>
<p>CPU pins constrain only <strong>completion</strong>: the minimum pending row across CPUs and machines is the point below which no future update can occur. The sink receives <code>OnStorageFrontierAdvance(id)</code> for eviction and <code>OnFrontierAdvance(id)</code> for completion. A consumer supporting back-patches need not retain provisional records in memory.</p>
<p>The replay tests compare 12,000 events with full storage. A fifth CPU stalls until the final event, then patches its long-evicted first row using its saved timestamp. Output is identical, and assertions verify that patches arrive below the storage frontier but never below completion.</p>
<label for="frontier-slider">Illustration: storage frontier <output id="frontier-request">8</output></label><br>
<input id="frontier-slider" type="range" min="0" max="24" value="8" style="width:100%">
<div id="frontier-rows"></div><p id="frontier-caption"></p>
<p class="muted">Illustration: producers can still patch rows 10 and 17, but their parser state holds everything needed to calculate those patches. Local storage can pass those rows; completion cannot pass row 10 until its producer finishes.</p>
<p class="callout"><strong>Prototype restrictions:</strong> the experimental automatic policy is for ftrace scheduling inputs only. Generic-kernel, ETW, Fuchsia and mixed scheduling producers are not covered. Thread-state history is also sink-only. A separate experimental_slice_args_streaming policy covers JSON/TrackEvent slices and args. Other slice importers remain unaudited; string dictionaries, distinct arg-set hashes and deferred translations are retained. SQL bounds aggregation is skipped in frontier mode. A stalled CPU may prevent completion from moving indefinitely, but no longer pins local sched storage.</p></section>
<section id="performance"><h2>CPU and RSS measurements</h2>PERFORMANCE<p>Saved thread-state working state removes historical table growth from the fixed-identity scheduling workloads. See the current measurements and scaling summaries below. A stalled CPU no longer pins history: storage advances while completion remains zero.</p><p>Full = retained columns without sinks; sink = retained columns with checksum sinks; drop = conservative column omission; frontier = drop plus sink-only scheduling/thread-state and saved working state. Fixed identities and forced sorter drains isolate historical table memory. External sink storage is excluded.</p><p>The fixed-identity slice scaling test compares one million and four million children under one still-open parent. Streaming history stays flat while full storage grows. A full-storage sink run confirms the same event checksum.</p>SLICE_PERFORMANCE SCALING</section>
<section id="validation"><h2>Validation and practical limits</h2><table><thead><tr><th>Check</th><th>Result</th><th>Evidence</th></tr></thead><tbody>VALIDATION_ROWS</tbody></table>
<p>The final unit suite includes streaming storage tests and paired full-storage versus omitted-storage sink replays for JSON and protobuf TrackEvent/scheduling traces. Replays apply cell updates and compare final records for every emitted table, excluding variable execution-time stats.</p>
<details><summary>Reproduction commands</summary><pre>CCACHE_DISABLE=1 tools/ninja -C out/mac_debug -j 6 perfetto_unittests trace_processor_shell
out/mac_debug/perfetto_trace_processor_unittests
out/mac_debug/perfetto_trace_processor_unittests --gtest_filter='DataframeStreamingTest.*:StreamingTablesTest.*'
.venv/bin/python3 tools/diff_test_trace_processor.py out/mac_debug/trace_processor_shell --name-filter '^(JsonParser|TablesSched|ProcessTracking|TrackEvent):'</pre></details>
<p><strong>Not demonstrated:</strong> whole-format streaming equivalence for every importer, production-trace CPU/RSS regression numbers, or bounded-memory parsing of arbitrary traces. The full-storage baseline and streaming modes use the same rebuilt release binary. The pre-existing release shell at another revision was not used.</p>
<p>String interning, parser dictionaries, unresolved EOF data and the sorter remain memory consumers. Sink strings are StringIds: resolve/copy them or retain the dictionary. The measurements isolate retention policies within one build. They do not measure default-path overhead against an unmodified checkout or model an expensive external sink.</p></section>
<section><h2>Current validation</h2><p>All 2,414 unit tests and 116 selected parser differential tests pass. GN header dependency checks, C++ formatting, HTML local links and embedded JavaScript syntax pass.</p><p><a href="tests-hand-sinks-all.log">Unit results</a> · <a href="diff-tests-hand-sinks.log">Parser differential results</a> · <a href="gn-check-hand-sinks.log">GN checks</a> · <a href="../include/perfetto/trace_processor/trace_parser_sinks.h">Hand-written public sinks</a> · <a href="../src/trace_processor/types/trace_parser_options.h">Private policy options</a></p><p>Current typed slice replay also matches retained-output checksums: one million children, repeated args, natural sorter checkpoints. Peak RSS 101.92 → 17.44 MiB; CPU 0.897 → 0.914 s. <a href="benchmark-slices-hand-sinks.json">Raw results</a>.</p></section><section><h2>Current counter measurements: normal sorter protocol</h2><p>Fresh process per mode, same release build, synthetic ordered counters on one track. Public mode is named slice in the diagnostic runner but enables counters automatically. The retained sink and typed public sink checksums match at each size.</p><table><tr><th>Rows</th><th>Mode</th><th>CPU seconds</th><th>Peak RSS MiB</th></tr><tr><td>1,000,000</td><td>full</td><td>0.151</td><td>59.84</td></tr><tr><td>1,000,000</td><td>sink</td><td>0.165</td><td>59.86</td></tr><tr><td>1,000,000</td><td>drop</td><td>0.165</td><td>59.70</td></tr><tr><td>1,000,000</td><td>slice</td><td>0.169</td><td>14.00</td></tr><tr><td>4,000,000</td><td>full</td><td>0.595</td><td>250.80</td></tr><tr><td>4,000,000</td><td>sink</td><td>0.653</td><td>250.28</td></tr><tr><td>4,000,000</td><td>slice</td><td>0.673</td><td>14.39</td></tr></table><p>Four million counters: retained storage grows to 251 MiB; parser-only output stays near 14 MiB. CPU is about 13% above the full-storage baseline and about 3% above retained storage with a checksum sink. Real trace measurements and expensive consumer costs remain unmeasured.</p></section><section><h2>Current public API: hand-written per-table sinks</h2><pre>class MyCounterSink : public CounterSink {
  base::Status OnInsert(uint32_t id, const Row&amp; row) override;
  base::Status OnValueUpdate(uint32_t id, double value) override;
  base::Status OnTrackIdUpdate(uint32_t id, uint32_t track_id) override;
};
MyCounterSink counters;
TraceParserSinks sinks;
sinks.counters = &amp;counters;
auto parser = TraceParser::CreateInstance(TraceParserConfig{}, sinks);
parser-&gt;Parse(blob_view);
parser-&gt;NotifyEndOfFile();</pre><p>Five hand-written public interfaces cover slices, args, sched, thread state and counters. Inserts carry typed rows; named update callbacks carry typed values. Sink objects must outlive the parser. String IDs are parser-local, and string views must be copied during the callback. Unrequested output is discarded. Consumer errors become sticky Parse/EOF errors.</p><p>No public Flush, owned-byte-array convenience overload, storage accessor, retention mask, batch size or construction hook. Configuration exposes only parsing/sorting options. Internal policies automatically omit audited cells and advance storage frontiers. A storage frontier permits later patches; a completion frontier promises no further patches below it.</p><p>Counters retain no value arrays in this entrypoint. Storage advances every 1,024 inserts. Completion remains zero because process-track resolution and backward-looking GPU values can patch old IDs. EventTracker now uses the returned ID directly instead of rereading the counter table. Other public table sinks, parser-core extraction and JSON export migration remain future work.</p><p><a href="trace-parser-api.html">Current API and limitations</a>; <a href="benchmark-counters-summary.json">counter CPU/RSS measurements</a>. Earlier performance tables are historical comparisons with explicit chunk drains; current measurements use normal tracing-service sort checkpoints and no public Flush.</p></section>
<section id="inventory"><h2>Column inventory</h2><p>Core schemas only; implicit IDs are always retained and aliases are excluded. Search by table or column name.</p><input id="schema-search" type="search" placeholder="Search tables or columns"><div class="schema"><table><thead><tr><th>Table</th><th>Retained</th><th>Omitted in conservative mode</th></tr></thead><tbody id="schema-body"></tbody></table></div><p><a href="schema-inventory.json">Raw inventory JSON</a></p></section>
<section><h2>Files to review</h2><ul>SOURCE_LINKS</ul><p><a href="build-debug.log">Build log</a></p></section>
<section id="journal"><h2>Decision journal</h2><p><a href="journal.txt">Plain-text journal</a></p><details><summary>Expand the full journal</summary><pre>JOURNAL</pre></details></section>
</main><script>
const inventory = INVENTORY_JSON;
function showInventory(query='') {
 const body=document.getElementById('schema-body');body.replaceChildren();
 for(const table of inventory){
  if(![table.table,...table.retained,...table.dropped].join(' ').toLowerCase().includes(query.toLowerCase()))continue;
  const row=document.createElement('tr');
  for(const value of [table.table,table.retained.join(', '),table.dropped.length?table.dropped.join(', '):'None']){
   const cell=document.createElement('td');cell.textContent=value;row.append(cell);
  }body.append(row);
 }
}
document.getElementById('schema-search').addEventListener('input',event=>showInventory(event.target.value));showInventory();
function showFrontier(){
 const requested=Number(document.getElementById('frontier-slider').value),complete=Math.min(requested,10);
 document.getElementById('frontier-request').textContent=requested;
 const container=document.getElementById('frontier-rows');container.replaceChildren();
 for(let id=0;id<24;id++){const row=document.createElement('div');row.className='row'+(id<requested?' gone':'')+([10,17].includes(id)?' pinned':'');row.textContent=id;container.append(row);}
 document.getElementById('frontier-caption').textContent=`Storage frontier: ${requested}. Completion frontier: ${complete}. Local retained rows: ${24-requested}. Patches to IDs at or above ${complete} remain valid. Next ID: 24.`;
}
document.getElementById('frontier-slider').addEventListener('input',showFrontier);showFrontier();
</script></body></html>'''
html = html.replace(
    'GENERATED',
    escape(datetime.now(timezone.utc).isoformat(timespec='seconds')))
html = html.replace('SLICE_PERFORMANCE',
                    slice_performance).replace('SCALING', scaling)
html = html.replace('PERFORMANCE', performance)
html = html.replace('VALIDATION_ROWS', rows).replace('SOURCE_LINKS', links)
html = html.replace('JOURNAL', escape(journal))
html = html.replace('INVENTORY_JSON',
                    json.dumps(inventory).replace('<', '\\u003c'))
(ROOT / 'catch-up.html').write_text(html)
print('Wrote', ROOT / 'catch-up.html')
