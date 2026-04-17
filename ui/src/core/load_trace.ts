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

import {assertExists, assertTrue} from '../base/assert';
import {sha1} from '../base/hash';
import {time, Time, TimeSpan} from '../base/time';
import {
  TraceBufferStream,
  TraceFileStream,
  TraceHttpStream,
  TraceMultipleFilesStream,
} from '../core/trace_stream';
import {TraceStream} from '../public/stream';
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
import {featureFlags} from './feature_flags';
import {
  getEnabledMetatracingCategories,
  isMetatracingEnabled,
} from './metatracing';
import {raf} from './raf_scheduler';
import {TraceSource} from './trace_source';

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
const FORCE_FULL_SORT_FLAG = featureFlags.register({
  id: 'forceFullSort',
  name: 'Force full sort',
  description:
    'Forces the trace processor into performing a full sort ignoring any windowing logic',
  defaultValue: false,
});

export interface RawTrace extends Disposable {
  readonly engine: EngineBase;
  readonly uuid: string;
  readonly traceSpan: TimeSpan;
  readonly reliableRange: TimeSpan;
  readonly tzOffMin: number;
  readonly unixOffset: time;
  readonly traceTypes: string[];
  readonly hasFtrace: boolean;
  readonly traceTitle: string;
  readonly traceUrl: string;
  readonly downloadable: boolean;
  readonly importErrors: number;
}

interface TraceLoadArgs {
  readonly useHttpIfAvailable?: boolean;
  readonly extraParsingDescriptors?: readonly Uint8Array[];
}

let lastEngineId = 0;

// Loads a trace from a given source and returns the loaded trace object
export async function loadTrace(
  traceSource: TraceSource,
  opts: TraceLoadArgs = {},
): Promise<RawTrace> {
  const {useHttpIfAvailable = false, extraParsingDescriptors = []} = opts;

  traceSource = maybeFixBuffers(traceSource);
  const engineId = `${++lastEngineId}`;
  const engine = await createEngine(
    engineId,
    useHttpIfAvailable,
    extraParsingDescriptors,
  );
  engine.onResponseReceived = function () {
    raf.scheduleFullRedraw();
  };

  await engine.resetTraceProcessor({
    tokenizeOnly: false,
    cropTrackEvents: CROP_TRACK_EVENTS_FLAG.get(),
    ingestFtraceInRawTable: INGEST_FTRACE_IN_RAW_TABLE_FLAG.get(),
    analyzeTraceProtoContent: ANALYZE_TRACE_PROTO_CONTENT_FLAG.get(),
    ftraceDropUntilAllCpusValid: FTRACE_DROP_UNTIL_FLAG.get(),
    forceFullSort: FORCE_FULL_SORT_FLAG.get(),
  });

  await loadTraceIntoEngine(engine, traceSource);

  const uuid = await getTraceUuid(engine);
  const traceTypes = await getTraceTypes(engine);
  const traceSpan = await getTraceSpan(engine);
  const hasJsonTrace = traceTypes.includes('json');
  const reliableRange = await computeVisibleTime(
    engine,
    traceSpan,
    hasJsonTrace,
  );
  const tzOffMin = await getTzOffset(engine);
  const unixOffset = await getUnixEpochOffset(engine);
  const hasFtrace = await getHasFtrace(engine);
  const {traceTitle, traceUrl} = getTraceTitleAndUrl(traceSource);
  const downloadable = isDownloadable(traceSource);
  const importErrors = await getTraceErrors(engine);
  await includeSummaryTables(engine);
  await defineMaxLayoutDepthSqlFunction(engine);

  return {
    engine,
    uuid,
    traceSpan,
    reliableRange,
    tzOffMin,
    unixOffset,
    traceTypes,
    hasFtrace,
    traceTitle,
    traceUrl,
    downloadable,
    importErrors,
    [Symbol.dispose]() {
      console.log(`Disposing trace: ${uuid}`);
      engine[Symbol.dispose]();
    },
  };
}

function maybeFixBuffers(src: TraceSource) {
  if (src.type === 'ARRAY_BUFFER' && src.buffer instanceof Uint8Array) {
    // Even though the type of `buffer` is ArrayBuffer, it's possible to
    // accidentally pass a Uint8Array here, because the interface of
    // Uint8Array is compatible with ArrayBuffer. That can cause subtle bugs
    // in TraceStream when creating chunks out of it (see b/390473162).
    // So if we get a Uint8Array in input, convert it into an actual
    // ArrayBuffer, as various parts of the codebase assume that this is a
    // pure ArrayBuffer, and not a logical view of it with a byteOffset > 0.
    const u8 = src.buffer as unknown as Uint8Array;
    if (u8.byteOffset === 0 && u8.byteLength === u8.buffer.byteLength) {
      src = {...src, buffer: u8.buffer as ArrayBuffer};
    } else {
      src = {...src, buffer: u8.slice().buffer as ArrayBuffer};
    }
  }
  return src;
}

async function createEngine(
  engineId: string,
  useHttpEngineIfAvailable: boolean,
  extraParsingDescriptors: readonly Uint8Array[] = [],
): Promise<EngineBase> {
  // Check if there is any instance of the trace_processor_shell running in
  // HTTP RPC mode (i.e. trace_processor_shell -D).
  const useRpc =
    useHttpEngineIfAvailable &&
    (await HttpRpcEngine.checkConnection()).connected;

  let engine;
  if (useRpc) {
    console.log('Opening trace using native accelerator over HTTP+RPC');
    engine = new HttpRpcEngine(engineId);
  } else {
    console.log('Opening trace using built-in WASM engine');
    engine = new WasmEngineProxy(engineId);
    engine.resetTraceProcessor({
      tokenizeOnly: false,
      cropTrackEvents: CROP_TRACK_EVENTS_FLAG.get(),
      ingestFtraceInRawTable: INGEST_FTRACE_IN_RAW_TABLE_FLAG.get(),
      analyzeTraceProtoContent: ANALYZE_TRACE_PROTO_CONTENT_FLAG.get(),
      ftraceDropUntilAllCpusValid: FTRACE_DROP_UNTIL_FLAG.get(),
      extraParsingDescriptors,
      forceFullSort: FORCE_FULL_SORT_FLAG.get(),
    });
  }
  engine.onResponseReceived = () => raf.scheduleFullRedraw();

  if (isMetatracingEnabled()) {
    engine.enableMetatrace(assertExists(getEnabledMetatracingCategories()));
  }
  return engine;
}

async function loadTraceIntoEngine(
  engine: EngineBase,
  traceSource: TraceSource,
): Promise<void> {
  const traceStream = createStreamFromSource(traceSource);

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
      console.log(status);
      if (res.eof) break;
    }
    await engine.notifyEof();
  } else {
    assertTrue(engine instanceof HttpRpcEngine);
    await engine.restoreInitialTables();
  }
}

function createStreamFromSource(
  traceSource: TraceSource,
): TraceStream | undefined {
  if (traceSource.type === 'FILE') {
    return new TraceFileStream(traceSource.file);
  } else if (traceSource.type === 'ARRAY_BUFFER') {
    return new TraceBufferStream(traceSource.buffer);
  } else if (traceSource.type === 'URL') {
    return new TraceHttpStream(traceSource.url);
  } else if (traceSource.type === 'STREAM') {
    return traceSource.stream;
  } else if (traceSource.type === 'HTTP_RPC') {
    return undefined;
  } else if (traceSource.type === 'MULTIPLE_FILES') {
    return new TraceMultipleFilesStream(traceSource.files);
  } else {
    throw new Error(`Unknown source: ${JSON.stringify(traceSource)}`);
  }
}

// TODO(sashwinbalaji): Move session UUID generation to TraceProcessor.
// getTraceUuid is a temporary measure to ensure multi-trace sessions have a
// unique cache key. This prevents collisions where a multi-trace session (e.g.
// a ZIP) would otherwise reuse the cache entry of its first component trace if
// that trace was previously opened individually.
async function getTraceUuid(engine: Engine): Promise<string> {
  // Each trace in the session contributes to the global cache key. To maintain
  // stable identifiers, we use the following priority:
  // 1. Per-trace UUID: e.g. from a TraceUuid packet.
  // 2. Global session UUID: ONLY used if no trace in the entire session has a
  //    specific UUID.
  // 3. Trace ID + Type: e.g. '1-perf'. The last-resort fallback.
  const uuidRes = await engine.query(`
    INCLUDE PERFETTO MODULE std.traceinfo.trace;
    SELECT DISTINCT
      coalesce(
        trace_uuid,
        iif(
          (SELECT COUNT(trace_uuid) FROM _metadata_by_trace) = 0,
          extract_metadata('trace_uuid'),
          NULL
        ),
        trace_id || '-' || trace_type
      ) AS uuid
    FROM _metadata_by_trace
  `);
  const uuids: string[] = [];
  for (
    const itUuid = uuidRes.iter({uuid: STR});
    itUuid.valid();
    itUuid.next()
  ) {
    uuids.push(itUuid.uuid);
  }

  if (uuids.length === 0) return '';
  if (uuids.length === 1) return uuids[0];
  const sortedUuids = [...uuids].sort();
  return await sha1(sortedUuids.join(';'));
}

async function getTraceTypes(engine: Engine): Promise<string[]> {
  const result = await engine.query(`
    INCLUDE PERFETTO MODULE std.traceinfo.trace;
    SELECT DISTINCT
      trace_type AS str_value
    FROM _metadata_by_trace
  `);

  const traceTypes: string[] = [];
  const it = result.iter({str_value: STR});
  for (; it.valid(); it.next()) {
    traceTypes.push(it.str_value);
  }
  return traceTypes;
}

async function getTraceSpan(engine: Engine): Promise<TimeSpan> {
  const result = await engine.query(`
    SELECT
      start_ts AS startTs,
      end_ts AS endTs
    FROM trace_bounds
  `);
  const bounds = result.firstRow({
    startTs: LONG,
    endTs: LONG,
  });
  return new TimeSpan(Time.fromRaw(bounds.startTs), Time.fromRaw(bounds.endTs));
}

async function computeVisibleTime(
  engine: Engine,
  traceSpan: TimeSpan,
  isJsonTrace: boolean,
): Promise<TimeSpan> {
  // initialise visible time to the trace time bounds
  let visibleStart = traceSpan.start;
  let visibleEnd = traceSpan.end;

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

async function getTracingMetadataTimeBounds(engine: Engine): Promise<TimeSpan> {
  const queryRes = await engine.query(`
    SELECT
      name,
      int_value AS intValue
    FROM metadata
    WHERE
      name = 'tracing_started_ns' OR
      name = 'tracing_disabled_ns' OR
      name = 'all_data_source_started_ns'
  `);
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

async function computeTraceReliableRangeStart(engine: Engine): Promise<time> {
  const result = await engine.query(`
    SELECT RUN_METRIC('chrome/chrome_reliable_range.sql');
    SELECT start
    FROM chrome_reliable_range
  `);
  const bounds = result.firstRow({start: LONG});
  return Time.fromRaw(bounds.start);
}

async function computeFtraceBounds(engine: Engine): Promise<TimeSpan | null> {
  const result = await engine.query(`
    SELECT
      MIN(ts) AS start,
      MAX(ts) AS end
    FROM ftrace_event
  `);
  const {start, end} = result.firstRow({start: LONG_NULL, end: LONG_NULL});
  if (start !== null && end !== null) {
    return new TimeSpan(Time.fromRaw(start), Time.fromRaw(end));
  }
  return null;
}

async function getTzOffset(engine: Engine): Promise<number> {
  // The max() is so the query returns NULL if the tz info doesn't exist.
  const result = await engine.query(`
    SELECT MAX(int_value) AS tzOffMin
    FROM metadata
    WHERE name = 'timezone_off_mins'
  `);
  return result.firstRow({tzOffMin: NUM_NULL}).tzOffMin ?? 0;
}

async function getUnixEpochOffset(engine: Engine): Promise<time> {
  // Find the first REALTIME or REALTIME_COARSE clock snapshot.
  // Prioritize REALTIME over REALTIME_COARSE.
  const result = await engine.query(`
    SELECT
      ts,
      clock_value AS clockValue,
      clock_name AS clockName
    FROM clock_snapshot
    WHERE
      snapshot_id = 0 AND
      clock_name IN ('REALTIME', 'REALTIME_COARSE')
  `);
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

  // This is the offset between the unix epoch and ts in the ts domain.
  // I.e. the value of ts at the time of the unix epoch - usually some large
  // negative value.
  return Time.sub(snapshot.ts, snapshot.clockValue);
}

async function getHasFtrace(engine: Engine): Promise<boolean> {
  const result = await engine.query(`
    SELECT *
    FROM ftrace_event
    LIMIT 1
  `);
  return result.numRows() > 0;
}

function getTraceTitleAndUrl(traceSource: TraceSource): {
  traceTitle: string;
  traceUrl: string;
} {
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

  return {traceTitle, traceUrl};
}

function isDownloadable(traceSource: TraceSource): boolean {
  return (
    (traceSource.type === 'ARRAY_BUFFER' && !traceSource.localOnly) ||
    traceSource.type === 'FILE' ||
    traceSource.type === 'URL'
  );
}

async function getTraceErrors(engine: Engine): Promise<number> {
  const result = await engine.query(`
    SELECT SUM(value) AS errs
    FROM stats
    WHERE severity != 'info'
  `);
  return result.firstRow({errs: NUM}).errs;
}

async function includeSummaryTables(engine: Engine): Promise<void> {
  await engine.query(`INCLUDE PERFETTO MODULE viz.summary.slices;`);
  await engine.query(`INCLUDE PERFETTO MODULE viz.summary.counters;`);
  await engine.query(`INCLUDE PERFETTO MODULE viz.summary.threads;`);
  await engine.query(`INCLUDE PERFETTO MODULE viz.summary.processes;`);
}

// TODO(stevegolton): Move this into some global "SQL extensions" file and
// ensure it's only run once.
async function defineMaxLayoutDepthSqlFunction(engine: Engine): Promise<void> {
  await engine.query(`
    CREATE PERFETTO FUNCTION __max_layout_depth(track_count INT, track_ids STRING)
    RETURNS INT AS
    SELECT iif(
      $track_count = 1,
      (
        SELECT max_depth
        FROM _slice_track_summary
        WHERE id = CAST($track_ids AS INT)
      ),
      (
        SELECT MAX(layout_depth)
        FROM experimental_slice_layout($track_ids)
      )
    );
  `);
}
