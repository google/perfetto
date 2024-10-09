// Copyright (C) 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {assertExists, assertTrue} from '../base/logging';
import {time, Time, TimeSpan} from '../base/time';
import {Actions} from '../common/actions';
import {cacheTrace} from '../core/cache_manager';
import {
  getEnabledMetatracingCategories,
  isMetatracingEnabled,
} from '../core/metatracing';
import {EngineConfig} from '../common/state';
import {featureFlags, Flag} from '../core/feature_flags';
import {globals} from '../frontend/globals';
import {Engine, EngineBase} from '../trace_processor/engine';
import {HttpRpcEngine} from '../trace_processor/http_rpc_engine';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  QueryError,
  STR,
  STR_NULL,
} from '../trace_processor/query_result';
import {WasmEngineProxy} from '../trace_processor/wasm_engine_proxy';
import {Controller} from './controller';
import {
  TraceBufferStream,
  TraceFileStream,
  TraceHttpStream,
  TraceStream,
} from '../core/trace_stream';
import {decideTracks} from '../core/track_decider';
import {
  deserializeAppStatePhase1,
  deserializeAppStatePhase2,
} from '../core/state_serialization';
import {TraceInfo} from '../public/trace_info';
import {AppImpl} from '../core/app_impl';
import {raf} from '../core/raf_scheduler';
import {TraceImpl} from '../core/trace_impl';
import {SerializedAppState} from '../public/state_serialization_schema';
import {TraceSource} from '../public/trace_source';
import {ThreadDesc} from '../public/threads';
import {Router} from '../core/router';

type States = 'init' | 'loading_trace' | 'ready';

const METRICS = [
  'android_ion',
  'android_lmk',
  'android_surfaceflinger',
  'android_batt',
  'android_other_traces',
  'chrome_dropped_frames',
  // TODO(289365196): Reenable:
  // 'chrome_long_latency',
  'android_trusty_workqueues',
];
const FLAGGED_METRICS: Array<[Flag, string]> = METRICS.map((m) => {
  const id = `forceMetric${m}`;
  let name = m.split('_').join(' ');
  name = name[0].toUpperCase() + name.slice(1);
  name = 'Metric: ' + name;
  const flag = featureFlags.register({
    id,
    name,
    description: `Overrides running the '${m}' metric at import time.`,
    defaultValue: true,
  });
  return [flag, m];
});

const ENABLE_CHROME_RELIABLE_RANGE_ZOOM_FLAG = featureFlags.register({
  id: 'enableChromeReliableRangeZoom',
  name: 'Enable Chrome reliable range zoom',
  description: 'Automatically zoom into the reliable range for Chrome traces',
  defaultValue: false,
});

const ENABLE_CHROME_RELIABLE_RANGE_ANNOTATION_FLAG = featureFlags.register({
  id: 'enableChromeReliableRangeAnnotation',
  name: 'Enable Chrome reliable range annotation',
  description: 'Automatically adds an annotation for the reliable range start',
  defaultValue: false,
});

// The following flags control TraceProcessor Config.
const CROP_TRACK_EVENTS_FLAG = featureFlags.register({
  id: 'cropTrackEvents',
  name: 'Crop track events',
  description: 'Ignores track events outside of the range of interest',
  defaultValue: false,
});
const INGEST_FTRACE_IN_RAW_TABLE_FLAG = featureFlags.register({
  id: 'ingestFtraceInRawTable',
  name: 'Ingest ftrace in raw table',
  description: 'Enables ingestion of typed ftrace events into the raw table',
  defaultValue: true,
});
const ANALYZE_TRACE_PROTO_CONTENT_FLAG = featureFlags.register({
  id: 'analyzeTraceProtoContent',
  name: 'Analyze trace proto content',
  description:
    'Enables trace proto content analysis (experimental_proto_content table)',
  defaultValue: false,
});
const FTRACE_DROP_UNTIL_FLAG = featureFlags.register({
  id: 'ftraceDropUntilAllCpusValid',
  name: 'Crop ftrace events',
  description:
    'Drop ftrace events until all per-cpu data streams are known to be valid',
  defaultValue: true,
});

// TODO(stevegolton): Move this into some global "SQL extensions" file and
// ensure it's only run once.
async function defineMaxLayoutDepthSqlFunction(engine: Engine): Promise<void> {
  await engine.query(`
    create perfetto function __max_layout_depth(track_count INT, track_ids STRING)
    returns INT AS
    select iif(
      $track_count = 1,
      (
        select max_depth
        from _slice_track_summary
        where id = cast($track_ids AS int)
      ),
      (
        select max(layout_depth)
        from experimental_slice_layout($track_ids)
      )
    );
  `);
}

// TraceController handles handshakes with the frontend for everything that
// concerns a single trace. It owns the WASM trace processor engine, handles
// tracks data and SQL queries. There is one TraceController instance for each
// trace opened in the UI (for now only one trace is supported).
export class TraceController extends Controller<States> {
  private trace?: TraceImpl = undefined;

  constructor(private engineCfg: EngineConfig) {
    super('init');
  }

  run() {
    switch (this.state) {
      case 'init':
        updateStatus('Opening trace');
        this.setState('loading_trace');
        loadTrace(this.engineCfg.source, this.engineCfg.id)
          .then((traceImpl) => {
            this.trace = traceImpl;
          })
          .catch((err) => {
            updateStatus(`${err}`);
            throw err;
          })
          .finally(() => {
            globals.dispatch(Actions.runControllers({}));
            AppImpl.instance.setIsLoadingTrace(false);
            AppImpl.instance.omnibox.reset(/* focus= */ false);
          });
        break;

      case 'loading_trace':
        // Stay in this state until loadTrace() returns and marks the engine as
        // ready.
        if (this.trace === undefined || AppImpl.instance.isLoadingTrace) {
          return;
        }
        this.setState('ready');
        break;

      case 'ready':
        return [];

      default:
        throw new Error(`unknown state ${this.state}`);
    }
    return;
  }

  onDestroy() {
    AppImpl.instance.plugins.onTraceClose();
    AppImpl.instance.closeCurrentTrace();
  }
}

// TODO(primiano): the extra indentation here is purely to help Gerrit diff
// detection algorithm. It can be re-formatted in the next CL.

async function loadTrace(
  traceSource: TraceSource,
  engineId: string,
): Promise<TraceImpl> {
  // TODO(primiano): in the next CL remember to invoke here clearState() because
  // the openActions (which today do that) will be gone.
  //  globals.dispatch(Actions.clearState({}));
  const engine = await createEngine(engineId);
  return await loadTraceIntoEngine(traceSource, engine);
}

async function createEngine(engineId: string): Promise<EngineBase> {
  // Check if there is any instance of the trace_processor_shell running in
  // HTTP RPC mode (i.e. trace_processor_shell -D).
  let useRpc = false;
  if (AppImpl.instance.newEngineMode === 'USE_HTTP_RPC_IF_AVAILABLE') {
    useRpc = (await HttpRpcEngine.checkConnection()).connected;
  }
  let engine;
  if (useRpc) {
    console.log('Opening trace using native accelerator over HTTP+RPC');
    engine = new HttpRpcEngine(engineId);
  } else {
    console.log('Opening trace using built-in WASM engine');
    engine = new WasmEngineProxy(engineId);
    engine.resetTraceProcessor({
      cropTrackEvents: CROP_TRACK_EVENTS_FLAG.get(),
      ingestFtraceInRawTable: INGEST_FTRACE_IN_RAW_TABLE_FLAG.get(),
      analyzeTraceProtoContent: ANALYZE_TRACE_PROTO_CONTENT_FLAG.get(),
      ftraceDropUntilAllCpusValid: FTRACE_DROP_UNTIL_FLAG.get(),
    });
  }
  engine.onResponseReceived = () => raf.scheduleFullRedraw();

  if (isMetatracingEnabled()) {
    engine.enableMetatrace(assertExists(getEnabledMetatracingCategories()));
  }
  return engine;
}

async function loadTraceIntoEngine(
  traceSource: TraceSource,
  engine: EngineBase,
): Promise<TraceImpl> {
  AppImpl.instance.setIsLoadingTrace(true);
  let traceStream: TraceStream | undefined;
  let serializedAppState: SerializedAppState | undefined;
  if (traceSource.type === 'FILE') {
    traceStream = new TraceFileStream(traceSource.file);
  } else if (traceSource.type === 'ARRAY_BUFFER') {
    traceStream = new TraceBufferStream(traceSource.buffer);
  } else if (traceSource.type === 'URL') {
    traceStream = new TraceHttpStream(traceSource.url);
    serializedAppState = traceSource.serializedAppState;
  } else if (traceSource.type === 'HTTP_RPC') {
    traceStream = undefined;
  } else {
    throw new Error(`Unknown source: ${JSON.stringify(traceSource)}`);
  }

  // |traceStream| can be undefined in the case when we are using the external
  // HTTP+RPC endpoint and the trace processor instance has already loaded
  // a trace (because it was passed as a cmdline argument to
  // trace_processor_shell). In this case we don't want the UI to load any
  // file/stream and we just want to jump to the loading phase.
  if (traceStream !== undefined) {
    const tStart = performance.now();
    for (;;) {
      const res = await traceStream.readChunk();
      await engine.parse(res.data);
      const elapsed = (performance.now() - tStart) / 1000;
      let status = 'Loading trace ';
      if (res.bytesTotal > 0) {
        const progress = Math.round((res.bytesRead / res.bytesTotal) * 100);
        status += `${progress}%`;
      } else {
        status += `${Math.round(res.bytesRead / 1e6)} MB`;
      }
      status += ` - ${Math.ceil(res.bytesRead / elapsed / 1e6)} MB/s`;
      updateStatus(status);
      if (res.eof) break;
    }
    await engine.notifyEof();
  } else {
    assertTrue(engine instanceof HttpRpcEngine);
    await engine.restoreInitialTables();
  }
  for (const p of globals.extraSqlPackages) {
    await engine.registerSqlPackages(p);
  }

  const traceDetails = await getTraceInfo(engine, traceSource);
  const trace = TraceImpl.createInstanceForCore(
    AppImpl.instance,
    engine,
    traceDetails,
  );
  AppImpl.instance.setActiveTrace(trace);

  const visibleTimeSpan = await computeVisibleTime(
    traceDetails.start,
    traceDetails.end,
    trace.traceInfo.traceType === 'json',
    engine,
  );

  trace.timeline.updateVisibleTime(visibleTimeSpan);

  const cacheUuid = traceDetails.cached ? traceDetails.uuid : '';
  Router.navigate(`#!/viewer?local_cache_key=${cacheUuid}`);

  // Make sure the helper views are available before we start adding tracks.
  await initialiseHelperViews(trace);
  await includeSummaryTables(engine);

  await defineMaxLayoutDepthSqlFunction(engine);

  if (serializedAppState !== undefined) {
    deserializeAppStatePhase1(serializedAppState, trace);
  }

  await AppImpl.instance.plugins.onTraceLoad(trace, (id) => {
    updateStatus(`Running plugin: ${id}`);
  });

  updateStatus('Loading tracks');
  await decideTracks(trace);

  decideTabs(trace);

  await listThreads(trace);

  // Trace Processor doesn't support the reliable range feature for JSON
  // traces.
  if (
    trace.traceInfo.traceType !== 'json' &&
    ENABLE_CHROME_RELIABLE_RANGE_ANNOTATION_FLAG.get()
  ) {
    const reliableRangeStart = await computeTraceReliableRangeStart(engine);
    if (reliableRangeStart > 0) {
      trace.notes.addNote({
        timestamp: reliableRangeStart,
        color: '#ff0000',
        text: 'Reliable Range Start',
      });
    }
  }

  if (serializedAppState !== undefined) {
    // Wait that plugins have completed their actions and then proceed with
    // the final phase of app state restore.
    // TODO(primiano): this can probably be removed once we refactor tracks
    // to be URI based and can deal with non-existing URIs.
    deserializeAppStatePhase2(serializedAppState, trace);
  }

  await trace.plugins.onTraceReady();

  return trace;
}

function decideTabs(trace: TraceImpl) {
  // Show the list of default tabs, but don't make them active!
  for (const tabUri of trace.tabs.defaultTabs) {
    trace.tabs.showTab(tabUri);
  }
}

async function listThreads(trace: TraceImpl) {
  updateStatus('Reading thread list');
  const query = `select
        utid,
        tid,
        pid,
        ifnull(thread.name, '') as threadName,
        ifnull(
          case when length(process.name) > 0 then process.name else null end,
          thread.name) as procName,
        process.cmdline as cmdline
        from (select * from thread order by upid) as thread
        left join (select * from process order by upid) as process
        using(upid)`;
  const result = await trace.engine.query(query);
  const threads = new Map<number, ThreadDesc>();
  const it = result.iter({
    utid: NUM,
    tid: NUM,
    pid: NUM_NULL,
    threadName: STR,
    procName: STR_NULL,
    cmdline: STR_NULL,
  });
  for (; it.valid(); it.next()) {
    const utid = it.utid;
    const tid = it.tid;
    const pid = it.pid === null ? undefined : it.pid;
    const threadName = it.threadName;
    const procName = it.procName === null ? undefined : it.procName;
    const cmdline = it.cmdline === null ? undefined : it.cmdline;
    threads.set(utid, {utid, tid, threadName, pid, procName, cmdline});
  }
  trace.setThreads(threads);
}

async function initialiseHelperViews(trace: TraceImpl) {
  const engine = trace.engine;
  updateStatus('Creating annotation counter track table');
  // Create the helper tables for all the annotations related data.
  // NULL in min/max means "figure it out per track in the usual way".
  await engine.query(`
      CREATE TABLE annotation_counter_track(
        id INTEGER PRIMARY KEY,
        name STRING,
        __metric_name STRING,
        upid INTEGER,
        min_value DOUBLE,
        max_value DOUBLE
      );
    `);
  updateStatus('Creating annotation slice track table');
  await engine.query(`
      CREATE TABLE annotation_slice_track(
        id INTEGER PRIMARY KEY,
        name STRING,
        __metric_name STRING,
        upid INTEGER,
        group_name STRING
      );
    `);

  updateStatus('Creating annotation counter table');
  await engine.query(`
      CREATE TABLE annotation_counter(
        id BIGINT,
        track_id INT,
        ts BIGINT,
        value DOUBLE,
        PRIMARY KEY (track_id, ts)
      ) WITHOUT ROWID;
    `);
  updateStatus('Creating annotation slice table');
  await engine.query(`
      CREATE TABLE annotation_slice(
        id INTEGER PRIMARY KEY,
        track_id INT,
        ts BIGINT,
        dur BIGINT,
        thread_dur BIGINT,
        depth INT,
        cat STRING,
        name STRING,
        UNIQUE(track_id, ts)
      );
    `);

  const availableMetrics = [];
  const metricsResult = await engine.query('select name from trace_metrics');
  for (const it = metricsResult.iter({name: STR}); it.valid(); it.next()) {
    availableMetrics.push(it.name);
  }

  const availableMetricsSet = new Set<string>(availableMetrics);
  for (const [flag, metric] of FLAGGED_METRICS) {
    if (!flag.get() || !availableMetricsSet.has(metric)) {
      continue;
    }

    updateStatus(`Computing ${metric} metric`);

    try {
      // We don't care about the actual result of metric here as we are just
      // interested in the annotation tracks.
      await engine.computeMetric([metric], 'proto');
    } catch (e) {
      if (e instanceof QueryError) {
        trace.addLoadingError('MetricError: ' + e.message);
        continue;
      } else {
        throw e;
      }
    }

    updateStatus(`Inserting data for ${metric} metric`);
    try {
      const result = await engine.query(`pragma table_info(${metric}_event)`);
      let hasSliceName = false;
      let hasDur = false;
      let hasUpid = false;
      let hasValue = false;
      let hasGroupName = false;
      const it = result.iter({name: STR});
      for (; it.valid(); it.next()) {
        const name = it.name;
        hasSliceName = hasSliceName || name === 'slice_name';
        hasDur = hasDur || name === 'dur';
        hasUpid = hasUpid || name === 'upid';
        hasValue = hasValue || name === 'value';
        hasGroupName = hasGroupName || name === 'group_name';
      }

      const upidColumnSelect = hasUpid ? 'upid' : '0 AS upid';
      const upidColumnWhere = hasUpid ? 'upid' : '0';
      const groupNameColumn = hasGroupName
        ? 'group_name'
        : 'NULL AS group_name';
      if (hasSliceName && hasDur) {
        await engine.query(`
            INSERT INTO annotation_slice_track(
              name, __metric_name, upid, group_name)
            SELECT DISTINCT
              track_name,
              '${metric}' as metric_name,
              ${upidColumnSelect},
              ${groupNameColumn}
            FROM ${metric}_event
            WHERE track_type = 'slice'
          `);
        await engine.query(`
            INSERT INTO annotation_slice(
              track_id, ts, dur, thread_dur, depth, cat, name
            )
            SELECT
              t.id AS track_id,
              ts,
              dur,
              NULL as thread_dur,
              0 AS depth,
              a.track_name as cat,
              slice_name AS name
            FROM ${metric}_event a
            JOIN annotation_slice_track t
            ON a.track_name = t.name AND t.__metric_name = '${metric}'
            ORDER BY t.id, ts
          `);
      }

      if (hasValue) {
        const minMax = await engine.query(`
            SELECT
              IFNULL(MIN(value), 0) as minValue,
              IFNULL(MAX(value), 0) as maxValue
            FROM ${metric}_event
            WHERE ${upidColumnWhere} != 0`);
        const row = minMax.firstRow({minValue: NUM, maxValue: NUM});
        await engine.query(`
            INSERT INTO annotation_counter_track(
              name, __metric_name, min_value, max_value, upid)
            SELECT DISTINCT
              track_name,
              '${metric}' as metric_name,
              CASE ${upidColumnWhere} WHEN 0 THEN NULL ELSE ${row.minValue} END,
              CASE ${upidColumnWhere} WHEN 0 THEN NULL ELSE ${row.maxValue} END,
              ${upidColumnSelect}
            FROM ${metric}_event
            WHERE track_type = 'counter'
          `);
        await engine.query(`
            INSERT INTO annotation_counter(id, track_id, ts, value)
            SELECT
              -1 as id,
              t.id AS track_id,
              ts,
              value
            FROM ${metric}_event a
            JOIN annotation_counter_track t
            ON a.track_name = t.name AND t.__metric_name = '${metric}'
            ORDER BY t.id, ts
          `);
      }
    } catch (e) {
      if (e instanceof QueryError) {
        trace.addLoadingError('MetricError: ' + e.message);
      } else {
        throw e;
      }
    }
  }
}

async function includeSummaryTables(engine: Engine) {
  updateStatus('Creating slice summaries');
  await engine.query(`include perfetto module viz.summary.slices;`);

  updateStatus('Creating counter summaries');
  await engine.query(`include perfetto module viz.summary.counters;`);

  updateStatus('Creating thread summaries');
  await engine.query(`include perfetto module viz.summary.threads;`);

  updateStatus('Creating processes summaries');
  await engine.query(`include perfetto module viz.summary.processes;`);

  updateStatus('Creating track summaries');
  await engine.query(`include perfetto module viz.summary.tracks;`);
}

function updateStatus(msg: string): void {
  const showUntilDismissed = 0;
  AppImpl.instance.omnibox.showStatusMessage(msg, showUntilDismissed);
}

async function computeFtraceBounds(engine: Engine): Promise<TimeSpan | null> {
  const result = await engine.query(`
    SELECT min(ts) as start, max(ts) as end FROM ftrace_event;
  `);
  const {start, end} = result.firstRow({start: LONG_NULL, end: LONG_NULL});
  if (start !== null && end !== null) {
    return new TimeSpan(Time.fromRaw(start), Time.fromRaw(end));
  }
  return null;
}

async function computeTraceReliableRangeStart(engine: Engine): Promise<time> {
  const result =
    await engine.query(`SELECT RUN_METRIC('chrome/chrome_reliable_range.sql');
       SELECT start FROM chrome_reliable_range`);
  const bounds = result.firstRow({start: LONG});
  return Time.fromRaw(bounds.start);
}

async function computeVisibleTime(
  traceStart: time,
  traceEnd: time,
  isJsonTrace: boolean,
  engine: Engine,
): Promise<TimeSpan> {
  // initialise visible time to the trace time bounds
  let visibleStart = traceStart;
  let visibleEnd = traceEnd;

  // compare start and end with metadata computed by the trace processor
  const mdTime = await getTracingMetadataTimeBounds(engine);
  // make sure the bounds hold
  if (Time.max(visibleStart, mdTime.start) < Time.min(visibleEnd, mdTime.end)) {
    visibleStart = Time.max(visibleStart, mdTime.start);
    visibleEnd = Time.min(visibleEnd, mdTime.end);
  }

  // Trace Processor doesn't support the reliable range feature for JSON
  // traces.
  if (!isJsonTrace && ENABLE_CHROME_RELIABLE_RANGE_ZOOM_FLAG.get()) {
    const reliableRangeStart = await computeTraceReliableRangeStart(engine);
    visibleStart = Time.max(visibleStart, reliableRangeStart);
  }

  // Move start of visible window to the first ftrace event
  const ftraceBounds = await computeFtraceBounds(engine);
  if (ftraceBounds !== null) {
    // Avoid moving start of visible window past its end!
    visibleStart = Time.min(ftraceBounds.start, visibleEnd);
  }
  return new TimeSpan(visibleStart, visibleEnd);
}

async function getTraceInfo(
  engine: Engine,
  traceSource: TraceSource,
): Promise<TraceInfo> {
  const traceTime = await getTraceTimeBounds(engine);

  // Find the first REALTIME or REALTIME_COARSE clock snapshot.
  // Prioritize REALTIME over REALTIME_COARSE.
  const query = `select
          ts,
          clock_value as clockValue,
          clock_name as clockName
        from clock_snapshot
        where
          snapshot_id = 0 AND
          clock_name in ('REALTIME', 'REALTIME_COARSE')
        `;
  const result = await engine.query(query);
  const it = result.iter({
    ts: LONG,
    clockValue: LONG,
    clockName: STR,
  });

  let snapshot = {
    clockName: '',
    ts: Time.ZERO,
    clockValue: Time.ZERO,
  };

  // Find the most suitable snapshot
  for (let row = 0; it.valid(); it.next(), row++) {
    if (it.clockName === 'REALTIME') {
      snapshot = {
        clockName: it.clockName,
        ts: Time.fromRaw(it.ts),
        clockValue: Time.fromRaw(it.clockValue),
      };
      break;
    } else if (it.clockName === 'REALTIME_COARSE') {
      if (snapshot.clockName !== 'REALTIME') {
        snapshot = {
          clockName: it.clockName,
          ts: Time.fromRaw(it.ts),
          clockValue: Time.fromRaw(it.clockValue),
        };
      }
    }
  }

  // The max() is so the query returns NULL if the tz info doesn't exist.
  const queryTz = `select max(int_value) as tzOffMin from metadata
        where name = 'timezone_off_mins'`;
  const resTz = await assertExists(engine).query(queryTz);
  const tzOffMin = resTz.firstRow({tzOffMin: NUM_NULL}).tzOffMin ?? 0;

  // This is the offset between the unix epoch and ts in the ts domain.
  // I.e. the value of ts at the time of the unix epoch - usually some large
  // negative value.
  const realtimeOffset = Time.sub(snapshot.ts, snapshot.clockValue);

  // Find the previous closest midnight from the trace start time.
  const utcOffset = Time.getLatestMidnight(traceTime.start, realtimeOffset);

  const traceTzOffset = Time.getLatestMidnight(
    traceTime.start,
    Time.sub(realtimeOffset, Time.fromSeconds(tzOffMin * 60)),
  );

  let traceTitle = '';
  let traceUrl = '';
  switch (traceSource.type) {
    case 'FILE':
      // Split on both \ and / (because C:\Windows\paths\are\like\this).
      traceTitle = traceSource.file.name.split(/[/\\]/).pop()!;
      const fileSizeMB = Math.ceil(traceSource.file.size / 1e6);
      traceTitle += ` (${fileSizeMB} MB)`;
      break;
    case 'URL':
      traceUrl = traceSource.url;
      traceTitle = traceUrl.split('/').pop()!;
      break;
    case 'ARRAY_BUFFER':
      traceTitle = traceSource.title;
      traceUrl = traceSource.url ?? '';
      const arrayBufferSizeMB = Math.ceil(traceSource.buffer.byteLength / 1e6);
      traceTitle += ` (${arrayBufferSizeMB} MB)`;
      break;
    case 'HTTP_RPC':
      traceTitle = `RPC @ ${HttpRpcEngine.hostAndPort}`;
      break;
    default:
      break;
  }

  const traceType = (
    await engine.query(
      `select str_value from metadata where name = 'trace_type'`,
    )
  ).firstRow({str_value: STR}).str_value;

  const hasFtrace =
    (await engine.query(`select * from ftrace_event limit 1`)).numRows() > 0;

  const uuidRes = await engine.query(`select str_value as uuid from metadata
    where name = 'trace_uuid'`);
  // trace_uuid can be missing from the TP tables if the trace is empty or in
  // other similar edge cases.
  const uuid = uuidRes.numRows() > 0 ? uuidRes.firstRow({uuid: STR}).uuid : '';
  const cached = await cacheTrace(traceSource, uuid);

  return {
    ...traceTime,
    traceTitle,
    traceUrl,
    realtimeOffset,
    utcOffset,
    traceTzOffset,
    cpus: await getCpus(engine),
    gpuCount: await getNumberOfGpus(engine),
    importErrors: await getTraceErrors(engine),
    source: traceSource,
    traceType,
    hasFtrace,
    uuid,
    cached,
  };
}

async function getTraceTimeBounds(engine: Engine): Promise<TimeSpan> {
  const result = await engine.query(
    `select start_ts as startTs, end_ts as endTs from trace_bounds`,
  );
  const bounds = result.firstRow({
    startTs: LONG,
    endTs: LONG,
  });
  return new TimeSpan(Time.fromRaw(bounds.startTs), Time.fromRaw(bounds.endTs));
}

// TODO(hjd): When streaming must invalidate this somehow.
async function getCpus(engine: Engine): Promise<number[]> {
  const cpus = [];
  const queryRes = await engine.query(
    'select distinct(cpu) as cpu from sched order by cpu;',
  );
  for (const it = queryRes.iter({cpu: NUM}); it.valid(); it.next()) {
    cpus.push(it.cpu);
  }
  return cpus;
}

async function getNumberOfGpus(engine: Engine): Promise<number> {
  const result = await engine.query(`
    select count(distinct(gpu_id)) as gpuCount
    from gpu_counter_track
    where name = 'gpufreq';
  `);
  return result.firstRow({gpuCount: NUM}).gpuCount;
}

async function getTraceErrors(engine: Engine): Promise<number> {
  const sql = `SELECT sum(value) as errs FROM stats WHERE severity != 'info'`;
  const result = await engine.query(sql);
  return result.firstRow({errs: NUM}).errs;
}

async function getTracingMetadataTimeBounds(engine: Engine): Promise<TimeSpan> {
  const queryRes = await engine.query(`select
       name,
       int_value as intValue
       from metadata
       where name = 'tracing_started_ns' or name = 'tracing_disabled_ns'
       or name = 'all_data_source_started_ns'`);
  let startBound = Time.MIN;
  let endBound = Time.MAX;
  const it = queryRes.iter({name: STR, intValue: LONG_NULL});
  for (; it.valid(); it.next()) {
    const columnName = it.name;
    const timestamp = it.intValue;
    if (timestamp === null) continue;
    if (columnName === 'tracing_disabled_ns') {
      endBound = Time.min(endBound, Time.fromRaw(timestamp));
    } else {
      startBound = Time.max(startBound, Time.fromRaw(timestamp));
    }
  }

  return new TimeSpan(startBound, endBound);
}
