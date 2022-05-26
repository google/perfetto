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

import { assertExists, assertTrue } from '../base/logging';
import {
  Actions,
  DeferredAction,
} from '../common/actions';
import { cacheTrace } from '../common/cache_manager';
import { TRACE_MARGIN_TIME_S } from '../common/constants';
import { Engine } from '../common/engine';
import { featureFlags, Flag, PERF_SAMPLE_FLAG } from '../common/feature_flags';
import { HttpRpcEngine } from '../common/http_rpc_engine';
import { NUM, NUM_NULL, QueryError, STR, STR_NULL } from '../common/query_result';
import {defaultTraceTime, EngineMode} from '../common/state';
import { TimeSpan, toNs, toNsCeil, toNsFloor } from '../common/time';
import { resetEngineWorker, WasmEngineProxy } from '../common/wasm_engine_proxy';
import {
  globals as frontendGlobals,
  QuantizedLoad,
  ThreadDesc
} from '../frontend/globals';
import {
  publishHasFtrace,
  publishMetricError,
  publishOverviewData,
  publishThreads
} from '../frontend/publish';
import { Router } from '../frontend/router';

import {
  CounterAggregationController
} from './aggregation/counter_aggregation_controller';
import {
  CpuAggregationController
} from './aggregation/cpu_aggregation_controller';
import {
  CpuByProcessAggregationController
} from './aggregation/cpu_by_process_aggregation_controller';
import {
  FrameAggregationController
} from './aggregation/frame_aggregation_controller';
import {
  SliceAggregationController
} from './aggregation/slice_aggregation_controller';
import {
  ThreadAggregationController
} from './aggregation/thread_aggregation_controller';
import { Child, Children, Controller } from './controller';
import {
  CpuProfileController,
  CpuProfileControllerArgs
} from './cpu_profile_controller';
import {
  FlamegraphController,
  FlamegraphControllerArgs
} from './flamegraph_controller';
import {
  FlowEventsController,
  FlowEventsControllerArgs
} from './flow_events_controller';
import { globals } from './globals';
import { LoadingManager } from './loading_manager';
import { LogsController } from './logs_controller';
import { MetricsController } from './metrics_controller';
import {PivotTableReduxController} from './pivot_table_redux_controller';
import {QueryController, QueryControllerArgs} from './query_controller';
import {SearchController} from './search_controller';
import {
  SelectionController,
  SelectionControllerArgs
} from './selection_controller';
import {
  TraceErrorController,
} from './trace_error_controller';
import {
  TraceBufferStream,
  TraceFileStream,
  TraceHttpStream,
  TraceStream
} from './trace_stream';
import { TrackControllerArgs, trackControllerRegistry } from './track_controller';
import { decideTracks } from './track_decider';

type States = 'init' | 'loading_trace' | 'ready';

const METRICS = [
  'android_startup',
  'android_ion',
  'android_lmk',
  'android_dma_heap',
  'android_surfaceflinger',
  'android_batt',
  'android_sysui_cuj',
  'android_jank',
  'android_camera',
  'android_other_traces',
  'chrome_dropped_frames',
  'chrome_long_latency',
  'trace_metadata',
  'android_trusty_workqueues',
];
const FLAGGED_METRICS: Array<[Flag, string]> = METRICS.map(m => {
  const id = `forceMetric${m}`;
  let name = m.split('_').join(' ') + ' metric';
  name = name[0].toUpperCase() + name.slice(1);
  const flag = featureFlags.register({
    id,
    name,
    description: `Overrides running the '${m}' metric at import time.`,
    defaultValue: true,
  });
  return [flag, m];
});

// TraceController handles handshakes with the frontend for everything that
// concerns a single trace. It owns the WASM trace processor engine, handles
// tracks data and SQL queries. There is one TraceController instance for each
// trace opened in the UI (for now only one trace is supported).
export class TraceController extends Controller<States> {
  private readonly engineId: string;
  private engine?: Engine;

  constructor(engineId: string) {
    super('init');
    this.engineId = engineId;
  }

  run() {
    const engineCfg = assertExists(globals.state.engines[this.engineId]);
    switch (this.state) {
      case 'init':
        this.loadTrace()
          .then(mode => {
            globals.dispatch(Actions.setEngineReady({
              engineId: this.engineId,
              ready: true,
              mode,
            }));
          })
          .catch(err => {
            this.updateStatus(`${err}`);
            throw err;
          });
        this.updateStatus('Opening trace');
        this.setState('loading_trace');
        break;

      case 'loading_trace':
        // Stay in this state until loadTrace() returns and marks the engine as
        // ready.
        if (this.engine === undefined || !engineCfg.ready) return;
        this.setState('ready');
        break;

      case 'ready':
        // At this point we are ready to serve queries and handle tracks.
        const engine = assertExists(this.engine);
        const childControllers: Children = [];

        // Create a TrackController for each track.
        for (const trackId of Object.keys(globals.state.tracks)) {
          const trackCfg = globals.state.tracks[trackId];
          if (trackCfg.engineId !== this.engineId) continue;
          if (!trackControllerRegistry.has(trackCfg.kind)) continue;
          const trackCtlFactory = trackControllerRegistry.get(trackCfg.kind);
          const trackArgs: TrackControllerArgs = { trackId, engine };
          childControllers.push(Child(trackId, trackCtlFactory, trackArgs));
        }

        // Create a QueryController for each query.
        for (const queryId of Object.keys(globals.state.queries)) {
          // If the expected engineId was not specified in the query, we
          // assume it's `state.currentEngineId`. The engineId is not specified
          // for instances with queries created prior to the creation of the
          // first engine.
          const expectedEngineId = globals.state.queries[queryId].engineId ||
              frontendGlobals.state.currentEngineId;
          // Check that we are executing the query on the correct engine.
          if (expectedEngineId !== engine.id) {
            continue;
          }
          const queryArgs: QueryControllerArgs = { queryId, engine };
          childControllers.push(Child(queryId, QueryController, queryArgs));
        }

        const selectionArgs: SelectionControllerArgs = { engine };
        childControllers.push(
          Child('selection', SelectionController, selectionArgs));

        const flowEventsArgs: FlowEventsControllerArgs = { engine };
        childControllers.push(
          Child('flowEvents', FlowEventsController, flowEventsArgs));

        const cpuProfileArgs: CpuProfileControllerArgs = { engine };
        childControllers.push(
          Child('cpuProfile', CpuProfileController, cpuProfileArgs));

        const flamegraphArgs: FlamegraphControllerArgs = { engine };
        childControllers.push(
          Child('flamegraph', FlamegraphController, flamegraphArgs));
        childControllers.push(Child(
          'cpu_aggregation',
          CpuAggregationController,
          { engine, kind: 'cpu_aggregation' }));
        childControllers.push(Child(
          'thread_aggregation',
          ThreadAggregationController,
          { engine, kind: 'thread_state_aggregation' }));
        childControllers.push(Child(
          'cpu_process_aggregation',
          CpuByProcessAggregationController,
          { engine, kind: 'cpu_by_process_aggregation' }));
        childControllers.push(Child(
          'slice_aggregation',
          SliceAggregationController,
          { engine, kind: 'slice_aggregation' }));
        childControllers.push(Child(
          'counter_aggregation',
          CounterAggregationController,
          { engine, kind: 'counter_aggregation' }));
        childControllers.push(Child(
          'frame_aggregation',
          FrameAggregationController,
          { engine, kind: 'frame_aggregation' }));
        childControllers.push(Child('search', SearchController, {
          engine,
          app: globals,
        }));
        childControllers.push(
            Child('pivot_table_redux', PivotTableReduxController, {engine}));

        childControllers.push(Child('logs', LogsController, {
          engine,
          app: globals,
        }));
        childControllers.push(
          Child('traceError', TraceErrorController, { engine }));
        childControllers.push(Child('metrics', MetricsController, { engine }));

        return childControllers;

      default:
        throw new Error(`unknown state ${this.state}`);
    }
    return;
  }

  onDestroy() {
    frontendGlobals.engines.delete(this.engineId);
  }

  private async loadTrace(): Promise<EngineMode> {
    this.updateStatus('Creating trace processor');
    // Check if there is any instance of the trace_processor_shell running in
    // HTTP RPC mode (i.e. trace_processor_shell -D).
    let engineMode: EngineMode;
    let useRpc = false;
    if (globals.state.newEngineMode === 'USE_HTTP_RPC_IF_AVAILABLE') {
      useRpc = (await HttpRpcEngine.checkConnection()).connected;
    }
    let engine;
    if (useRpc) {
      console.log('Opening trace using native accelerator over HTTP+RPC');
      engineMode = 'HTTP_RPC';
      engine = new HttpRpcEngine(this.engineId, LoadingManager.getInstance);
      engine.errorHandler = (err) => {
        globals.dispatch(
          Actions.setEngineFailed({ mode: 'HTTP_RPC', failure: `${err}` }));
        throw err;
      };
    } else {
      console.log('Opening trace using built-in WASM engine');
      engineMode = 'WASM';
      const enginePort = resetEngineWorker();
      engine = new WasmEngineProxy(
        this.engineId, enginePort, LoadingManager.getInstance);
    }
    this.engine = engine;

    frontendGlobals.engines.set(this.engineId, engine);
    globals.dispatch(Actions.setEngineReady({
      engineId: this.engineId,
      ready: false,
      mode: engineMode,
    }));
    const engineCfg = assertExists(globals.state.engines[this.engineId]);
    let traceStream: TraceStream | undefined;
    if (engineCfg.source.type === 'FILE') {
      traceStream = new TraceFileStream(engineCfg.source.file);
    } else if (engineCfg.source.type === 'ARRAY_BUFFER') {
      traceStream = new TraceBufferStream(engineCfg.source.buffer);
    } else if (engineCfg.source.type === 'URL') {
      traceStream = new TraceHttpStream(engineCfg.source.url);
    } else if (engineCfg.source.type === 'HTTP_RPC') {
      traceStream = undefined;
    } else {
      throw new Error(`Unknown source: ${JSON.stringify(engineCfg.source)}`);
    }

    // |traceStream| can be undefined in the case when we are using the external
    // HTTP+RPC endpoint and the trace processor instance has already loaded
    // a trace (because it was passed as a cmdline argument to
    // trace_processor_shell). In this case we don't want the UI to load any
    // file/stream and we just want to jump to the loading phase.
    if (traceStream !== undefined) {
      const tStart = performance.now();
      for (; ;) {
        const res = await traceStream.readChunk();
        await this.engine.parse(res.data);
        const elapsed = (performance.now() - tStart) / 1000;
        let status = 'Loading trace ';
        if (res.bytesTotal > 0) {
          const progress = Math.round(res.bytesRead / res.bytesTotal * 100);
          status += `${progress}%`;
        } else {
          status += `${Math.round(res.bytesRead / 1e6)} MB`;
        }
        status += ` - ${Math.ceil(res.bytesRead / elapsed / 1e6)} MB/s`;
        this.updateStatus(status);
        if (res.eof) break;
      }
      await this.engine.notifyEof();
    } else {
      assertTrue(this.engine instanceof HttpRpcEngine);
      await this.engine.restoreInitialTables();
    }

    // traceUuid will be '' if the trace is not cacheable (URL or RPC).
    const traceUuid = await this.cacheCurrentTrace();

    const traceTime = await this.engine.getTraceTimeBounds();
    let startSec = traceTime.start;
    let endSec = traceTime.end;
    startSec -= TRACE_MARGIN_TIME_S;
    endSec += TRACE_MARGIN_TIME_S;
    const traceTimeState = {
      startSec,
      endSec,
    };

    const emptyOmniboxState = {
      omnibox: '',
      mode: frontendGlobals.state.frontendLocalState.omniboxState.mode ||
        'SEARCH',
      lastUpdate: Date.now() / 1000
    };

    const actions: DeferredAction[] = [
      Actions.setOmnibox(emptyOmniboxState),
      Actions.setTraceUuid({ traceUuid }),
      Actions.setTraceTime(traceTimeState)
    ];

    const [startVisibleTime, endVisibleTime] =
        await computeVisibleTime(startSec, endSec, this.engine);
    // We don't know the resolution at this point. However this will be
    // replaced in 50ms so a guess is fine.
    const resolution = (endVisibleTime - startVisibleTime) / 1000;
    actions.push(Actions.setVisibleTraceTime({
      startSec: startVisibleTime,
      endSec: endVisibleTime,
      lastUpdate: Date.now() / 1000,
      resolution
    }));

    globals.dispatchMultiple(actions);
    Router.navigate(`#!/viewer?local_cache_key=${traceUuid}`);

    // Make sure the helper views are available before we start adding tracks.
    await this.initialiseHelperViews();

    {
      // When we reload from a permalink don't create extra tracks:
      const { pinnedTracks, tracks } = globals.state;
      if (!pinnedTracks.length && !Object.keys(tracks).length) {
        await this.listTracks();
      }
    }

    await this.listThreads();
    await this.loadTimelineOverview(traceTime);

    {
      // A quick heuristic to check if the trace has ftrace events. This is
      // based on the assumption that most traces that have ftrace either:
      // - Are proto traces captured via perfetto, in which case traced_probes
      //   emits ftrace per-cpu stats that end up in the stats table.
      // - Have a raw event with non-zero cpu or utid.
      // Notes:
      // - The "+1 > 1" is to avoid pushing down the constraints to the "raw"
      //   table, which would compute a full column filter without being aware
      //   of the limit 1, and instead delegate the filtering to the iterator.
      const query = `select '_' as _ from raw
          where cpu + 1 > 1 or utid + 1 > 1 limit 1`;
      const result = await assertExists(this.engine).query(query);
      const hasFtrace = result.numRows() > 0;
      publishHasFtrace(hasFtrace);
    }

    globals.dispatch(Actions.removeDebugTrack({}));
    globals.dispatch(Actions.sortThreadTracks({}));
    globals.dispatch(Actions.maybeExpandOnlyTrackGroup({}));

    await this.selectFirstHeapProfile();
    if (PERF_SAMPLE_FLAG.get()) {
      await this.selectPerfSample();
    }

    return engineMode;
  }

  private async selectPerfSample() {
    const query = `select ts, upid
        from perf_sample
        join thread using (utid)
        order by ts desc limit 1`;
    const profile = await assertExists(this.engine).query(query);
    if (profile.numRows() !== 1) return;
    const row = profile.firstRow({ ts: NUM, upid: NUM });
    const ts = row.ts;
    const upid = row.upid;
    globals.dispatch(
      Actions.selectPerfSamples({ id: 0, upid, ts, type: 'perf' }));
  }

  private async selectFirstHeapProfile() {
    const query = `select * from
    (select distinct(ts) as ts, 'native' as type,
        upid from heap_profile_allocation
        union
        select distinct(graph_sample_ts) as ts, 'graph' as type, upid from
        heap_graph_object) order by ts limit 1`;
    const profile = await assertExists(this.engine).query(query);
    if (profile.numRows() !== 1) return;
    const row = profile.firstRow({ ts: NUM, type: STR, upid: NUM });
    const ts = row.ts;
    const type = row.type;
    const upid = row.upid;
    globals.dispatch(Actions.selectHeapProfile({ id: 0, upid, ts, type }));
  }

  private async listTracks() {
    this.updateStatus('Loading tracks');
    const engine = assertExists<Engine>(this.engine);
    const actions = await decideTracks(this.engineId, engine);
    globals.dispatchMultiple(actions);
  }

  private async listThreads() {
    this.updateStatus('Reading thread list');
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
    const result = await assertExists(this.engine).query(query);
    const threads: ThreadDesc[] = [];
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
      threads.push({ utid, tid, threadName, pid, procName, cmdline });
    }
    publishThreads(threads);
  }

  private async loadTimelineOverview(traceTime: TimeSpan) {
    const engine = assertExists<Engine>(this.engine);
    const numSteps = 100;
    const stepSec = traceTime.duration / numSteps;
    let hasSchedOverview = false;
    for (let step = 0; step < numSteps; step++) {
      this.updateStatus(
        'Loading overview ' +
        `${Math.round((step + 1) / numSteps * 1000) / 10}%`);
      const startSec = traceTime.start + step * stepSec;
      const startNs = toNsFloor(startSec);
      const endSec = startSec + stepSec;
      const endNs = toNsCeil(endSec);

      // Sched overview.
      const schedResult = await engine.query(
        `select sum(dur)/${stepSec}/1e9 as load, cpu from sched ` +
        `where ts >= ${startNs} and ts < ${endNs} and utid != 0 ` +
        'group by cpu order by cpu');
      const schedData: { [key: string]: QuantizedLoad } = {};
      const it = schedResult.iter({ load: NUM, cpu: NUM });
      for (; it.valid(); it.next()) {
        const load = it.load;
        const cpu = it.cpu;
        schedData[cpu] = { startSec, endSec, load };
        hasSchedOverview = true;
      }
      publishOverviewData(schedData);
    }

    if (hasSchedOverview) {
      return;
    }

    // Slices overview.
    const traceStartNs = toNs(traceTime.start);
    const stepSecNs = toNs(stepSec);
    const sliceResult = await engine.query(`select
           bucket,
           upid,
           ifnull(sum(utid_sum) / cast(${stepSecNs} as float), 0) as load
         from thread
         inner join (
           select
             ifnull(cast((ts - ${traceStartNs})/${stepSecNs} as int), 0) as bucket,
             sum(dur) as utid_sum,
             utid
           from slice
           inner join thread_track on slice.track_id = thread_track.id
           group by bucket, utid
         ) using(utid)
         where upid is not null
         group by bucket, upid`);

    const slicesData: { [key: string]: QuantizedLoad[] } = {};
    const it = sliceResult.iter({ bucket: NUM, upid: NUM, load: NUM });
    for (; it.valid(); it.next()) {
      const bucket = it.bucket;
      const upid = it.upid;
      const load = it.load;

      const startSec = traceTime.start + stepSec * bucket;
      const endSec = startSec + stepSec;

      const upidStr = upid.toString();
      let loadArray = slicesData[upidStr];
      if (loadArray === undefined) {
        loadArray = slicesData[upidStr] = [];
      }
      loadArray.push({ startSec, endSec, load });
    }
    publishOverviewData(slicesData);
  }

  private async cacheCurrentTrace(): Promise<string> {
    const engine = assertExists(this.engine);
    const result = await engine.query(`select str_value as uuid from metadata
                  where name = 'trace_uuid'`);
    if (result.numRows() === 0) {
      // One of the cases covered is an empty trace.
      return '';
    }
    const traceUuid = result.firstRow({ uuid: STR }).uuid;
    const engineConfig = assertExists(globals.state.engines[engine.id]);
    if (!(await cacheTrace(engineConfig.source, traceUuid))) {
      // If the trace is not cacheable (cacheable means it has been opened from
      // URL or RPC) only append '?local_cache_key' to the URL, without the
      // local_cache_key value. Doing otherwise would cause an error if the tab
      // is discarded or the user hits the reload button because the trace is
      // not in the cache.
      return '';
    }
    return traceUuid;
  }

  async initialiseHelperViews() {
    const engine = assertExists<Engine>(this.engine);

    this.updateStatus('Creating annotation counter track table');
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
    this.updateStatus('Creating annotation slice track table');
    await engine.query(`
      CREATE TABLE annotation_slice_track(
        id INTEGER PRIMARY KEY,
        name STRING,
        __metric_name STRING,
        upid INTEGER,
        group_name STRING
      );
    `);

    this.updateStatus('Creating annotation counter table');
    await engine.query(`
      CREATE TABLE annotation_counter(
        id BIG INT,
        track_id INT,
        ts BIG INT,
        value DOUBLE,
        PRIMARY KEY (track_id, ts)
      ) WITHOUT ROWID;
    `);
    this.updateStatus('Creating annotation slice table');
    await engine.query(`
      CREATE TABLE annotation_slice(
        id INTEGER PRIMARY KEY,
        track_id INT,
        ts BIG INT,
        dur BIG INT,
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
    globals.dispatch(Actions.setAvailableMetrics({availableMetrics}));

    const availableMetricsSet = new Set<string>(availableMetrics);
    for (const [flag, metric] of FLAGGED_METRICS) {
      if (!flag.get() || !availableMetricsSet.has(metric)) {
        continue;
      }

      this.updateStatus(`Computing ${metric} metric`);
      try {
        // We don't care about the actual result of metric here as we are just
        // interested in the annotation tracks.
        await engine.computeMetric([metric]);
      } catch (e) {
        if (e instanceof QueryError) {
          publishMetricError('MetricError: ' + e.message);
          continue;
        } else {
          throw e;
        }
      }

      this.updateStatus(`Inserting data for ${metric} metric`);
      try {
        const result = await engine.query(`pragma table_info(${metric}_event)`);
        let hasSliceName = false;
        let hasDur = false;
        let hasUpid = false;
        let hasValue = false;
        let hasGroupName = false;
        const it = result.iter({ name: STR });
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
        const groupNameColumn =
          hasGroupName ? 'group_name' : 'NULL AS group_name';
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
            INSERT INTO annotation_slice(track_id, ts, dur, depth, cat, name)
            SELECT
              t.id AS track_id,
              ts,
              dur,
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
          const row = minMax.firstRow({ minValue: NUM, maxValue: NUM });
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
          publishMetricError('MetricError: ' + e.message);
        } else {
          throw e;
        }
      }
    }
  }

  private updateStatus(msg: string): void {
    globals.dispatch(Actions.updateStatus({
      msg,
      timestamp: Date.now() / 1000,
    }));
  }
}

async function computeVisibleTime(
    traceStartSec: number, traceEndSec: number, engine: Engine):
    Promise<[number, number]> {
  // if we have non-default visible state, update the visible time to it
  const previousVisibleState = globals.state.frontendLocalState.visibleState;
  if (!(previousVisibleState.startSec === defaultTraceTime.startSec &&
        previousVisibleState.endSec === defaultTraceTime.endSec)) {
    return [previousVisibleState.startSec, previousVisibleState.endSec];
  }

  // initialise visible time to the trace time bounds
  let visibleStartSec = traceStartSec;
  let visibleEndSec = traceEndSec;

  // compare start and end with metadata computed by the trace processor
  const mdTime = await engine.getTracingMetadataTimeBounds();
  // make sure the bounds hold
  if (Math.max(visibleStartSec, mdTime.start - TRACE_MARGIN_TIME_S) <
      Math.min(visibleEndSec, mdTime.end + TRACE_MARGIN_TIME_S)) {
    visibleStartSec =
        Math.max(visibleStartSec, mdTime.start - TRACE_MARGIN_TIME_S);
    visibleEndSec = Math.min(visibleEndSec, mdTime.end + TRACE_MARGIN_TIME_S);
  }
  return [visibleStartSec, visibleEndSec];
}
