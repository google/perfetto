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
import {cacheTrace} from './cache_manager';
import {
  getEnabledMetatracingCategories,
  isMetatracingEnabled,
} from './metatracing';
import {featureFlags} from './feature_flags';
import {Engine, EngineBase} from '../trace_processor/engine';
import {HttpRpcEngine} from '../trace_processor/http_rpc_engine';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
} from '../trace_processor/query_result';
import {WasmEngineProxy} from '../trace_processor/wasm_engine_proxy';
import {
  TraceBufferStream,
  TraceFileStream,
  TraceHttpStream,
  TraceStream,
} from '../core/trace_stream';
import {
  deserializeAppStatePhase1,
  deserializeAppStatePhase2,
} from './state_serialization';
import {AppImpl} from './app_impl';
import {raf} from './raf_scheduler';
import {TraceImpl} from './trace_impl';
import {SerializedAppState} from './state_serialization_schema';
import {TraceSource} from './trace_source';
import {Router} from '../core/router';
import {TraceInfoImpl} from './trace_info_impl';

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

let lastEngineId = 0;

export async function loadTrace(
  app: AppImpl,
  traceSource: TraceSource,
): Promise<TraceImpl> {
  updateStatus(app, 'Opening trace');
  const engineId = `${++lastEngineId}`;
  const engine = await createEngine(app, engineId);
  return await loadTraceIntoEngine(app, traceSource, engine);
}

async function createEngine(
  app: AppImpl,
  engineId: string,
): Promise<EngineBase> {
  // Check if there is any instance of the trace_processor_shell running in
  // HTTP RPC mode (i.e. trace_processor_shell -D).
  let useRpc = false;
  if (app.httpRpc.newEngineMode === 'USE_HTTP_RPC_IF_AVAILABLE') {
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
  app: AppImpl,
  traceSource: TraceSource,
  engine: EngineBase,
): Promise<TraceImpl> {
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
      updateStatus(app, status);
      if (res.eof) break;
    }
    await engine.notifyEof();
  } else {
    assertTrue(engine instanceof HttpRpcEngine);
    await engine.restoreInitialTables();
  }
  for (const p of app.extraSqlPackages) {
    await engine.registerSqlPackages(p);
  }

  const traceDetails = await getTraceInfo(engine, traceSource);
  const trace = TraceImpl.createInstanceForCore(app, engine, traceDetails);
  app.setActiveTrace(trace);

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
  await includeSummaryTables(trace);

  await defineMaxLayoutDepthSqlFunction(engine);

  if (serializedAppState !== undefined) {
    deserializeAppStatePhase1(serializedAppState, trace);
  }

  await app.plugins.onTraceLoad(trace, (id) => {
    updateStatus(app, `Running plugin: ${id}`);
  });

  decideTabs(trace);

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

  for (const callback of trace.getEventListeners('traceready')) {
    await callback();
  }

  if (serializedAppState !== undefined) {
    // Wait that plugins have completed their actions and then proceed with
    // the final phase of app state restore.
    // TODO(primiano): this can probably be removed once we refactor tracks
    // to be URI based and can deal with non-existing URIs.
    deserializeAppStatePhase2(serializedAppState, trace);
  }

  return trace;
}

function decideTabs(trace: TraceImpl) {
  // Show the list of default tabs, but don't make them active!
  for (const tabUri of trace.tabs.defaultTabs) {
    trace.tabs.showTab(tabUri);
  }
}

async function includeSummaryTables(trace: TraceImpl) {
  const engine = trace.engine;
  updateStatus(trace, 'Creating slice summaries');
  await engine.query(`include perfetto module viz.summary.slices;`);

  updateStatus(trace, 'Creating counter summaries');
  await engine.query(`include perfetto module viz.summary.counters;`);

  updateStatus(trace, 'Creating thread summaries');
  await engine.query(`include perfetto module viz.summary.threads;`);

  updateStatus(trace, 'Creating processes summaries');
  await engine.query(`include perfetto module viz.summary.processes;`);

  updateStatus(trace, 'Creating track summaries');
  await engine.query(`include perfetto module viz.summary.tracks;`);
}

function updateStatus(traceOrApp: TraceImpl | AppImpl, msg: string): void {
  const showUntilDismissed = 0;
  traceOrApp.omnibox.showStatusMessage(msg, showUntilDismissed);
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
): Promise<TraceInfoImpl> {
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

  const traceType = await getTraceType(engine);

  const hasFtrace =
    (await engine.query(`select * from ftrace_event limit 1`)).numRows() > 0;

  const uuidRes = await engine.query(`select str_value as uuid from metadata
    where name = 'trace_uuid'`);
  // trace_uuid can be missing from the TP tables if the trace is empty or in
  // other similar edge cases.
  const uuid = uuidRes.numRows() > 0 ? uuidRes.firstRow({uuid: STR}).uuid : '';
  const cached = await cacheTrace(traceSource, uuid);

  const downloadable =
    (traceSource.type === 'ARRAY_BUFFER' && !traceSource.localOnly) ||
    traceSource.type === 'FILE' ||
    traceSource.type === 'URL';

  return {
    ...traceTime,
    traceTitle,
    traceUrl,
    realtimeOffset,
    utcOffset,
    traceTzOffset,
    cpus: await getCpus(engine),
    importErrors: await getTraceErrors(engine),
    source: traceSource,
    traceType,
    hasFtrace,
    uuid,
    cached,
    downloadable,
  };
}

async function getTraceType(engine: Engine) {
  const result = await engine.query(
    `select str_value from metadata where name = 'trace_type'`,
  );

  if (result.numRows() === 0) return undefined;
  return result.firstRow({str_value: STR}).str_value;
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
