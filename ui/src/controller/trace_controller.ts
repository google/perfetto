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

import '../tracks/all_controller';

import {assertExists, assertTrue} from '../base/logging';
import {
  Actions,
  DeferredAction,
} from '../common/actions';
import {Engine, QueryError} from '../common/engine';
import {HttpRpcEngine} from '../common/http_rpc_engine';
import {slowlyCountRows} from '../common/query_iterator';
import {EngineMode} from '../common/state';
import {toNs, toNsCeil, toNsFloor} from '../common/time';
import {TimeSpan} from '../common/time';
import {
  createWasmEngine,
  destroyWasmEngine,
  WasmEngineProxy
} from '../common/wasm_engine_proxy';
import {QuantizedLoad, ThreadDesc} from '../frontend/globals';

import {
  CounterAggregationController
} from './aggregation/counter_aggregation_controller';
import {
  CpuAggregationController
} from './aggregation/cpu_aggregation_controller';
import {
  SliceAggregationController
} from './aggregation/slice_aggregation_controller';
import {
  ThreadAggregationController
} from './aggregation/thread_aggregation_controller';
import {Child, Children, Controller} from './controller';
import {
  CpuProfileController,
  CpuProfileControllerArgs
} from './cpu_profile_controller';
import {
  FlowEventsController,
  FlowEventsControllerArgs
} from './flow_events_controller';
import {globals} from './globals';
import {
  HeapProfileController,
  HeapProfileControllerArgs
} from './heap_profile_controller';
import {LoadingManager} from './loading_manager';
import {LogsController} from './logs_controller';
import {MetricsController} from './metrics_controller';
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
import {TrackControllerArgs, trackControllerRegistry} from './track_controller';
import {decideTracks} from './track_decider';

type States = 'init'|'loading_trace'|'ready';

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

  onDestroy() {
    if (this.engine instanceof WasmEngineProxy) {
      destroyWasmEngine(this.engine.id);
    }
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
        assertTrue(engineCfg.ready);
        const childControllers: Children = [];

        // Create a TrackController for each track.
        for (const trackId of Object.keys(globals.state.tracks)) {
          const trackCfg = globals.state.tracks[trackId];
          if (trackCfg.engineId !== this.engineId) continue;
          if (!trackControllerRegistry.has(trackCfg.kind)) continue;
          const trackCtlFactory = trackControllerRegistry.get(trackCfg.kind);
          const trackArgs: TrackControllerArgs = {trackId, engine};
          childControllers.push(Child(trackId, trackCtlFactory, trackArgs));
        }

        // Create a QueryController for each query.
        for (const queryId of Object.keys(globals.state.queries)) {
          const queryArgs: QueryControllerArgs = {queryId, engine};
          childControllers.push(Child(queryId, QueryController, queryArgs));
        }

        const selectionArgs: SelectionControllerArgs = {engine};
        childControllers.push(
            Child('selection', SelectionController, selectionArgs));

        const flowEventsArgs: FlowEventsControllerArgs = {engine};
        childControllers.push(
            Child('flowEvents', FlowEventsController, flowEventsArgs));

        const cpuProfileArgs: CpuProfileControllerArgs = {engine};
        childControllers.push(
            Child('cpuProfile', CpuProfileController, cpuProfileArgs));

        const heapProfileArgs: HeapProfileControllerArgs = {engine};
        childControllers.push(
            Child('heapProfile', HeapProfileController, heapProfileArgs));
        childControllers.push(Child(
            'cpu_aggregation',
            CpuAggregationController,
            {engine, kind: 'cpu_aggregation'}));
        childControllers.push(Child(
            'thread_aggregation',
            ThreadAggregationController,
            {engine, kind: 'thread_state_aggregation'}));
        childControllers.push(Child(
            'slice_aggregation',
            SliceAggregationController,
            {engine, kind: 'slice_aggregation'}));
        childControllers.push(Child(
            'counter_aggregation',
            CounterAggregationController,
            {engine, kind: 'counter_aggregation'}));
        childControllers.push(Child('search', SearchController, {
          engine,
          app: globals,
        }));

        childControllers.push(Child('logs', LogsController, {
          engine,
          app: globals,
        }));
        childControllers.push(
            Child('traceError', TraceErrorController, {engine}));
        childControllers.push(Child('metrics', MetricsController, {engine}));
        return childControllers;

      default:
        throw new Error(`unknown state ${this.state}`);
    }
    return;
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
    if (useRpc) {
      console.log('Opening trace using native accelerator over HTTP+RPC');
      engineMode = 'HTTP_RPC';
      const engine =
          new HttpRpcEngine(this.engineId, LoadingManager.getInstance);
      engine.errorHandler = (err) => {
        globals.dispatch(
            Actions.setEngineFailed({mode: 'HTTP_RPC', failure: `${err}`}));
        throw err;
      };
      this.engine = engine;
    } else {
      console.log('Opening trace using built-in WASM engine');
      engineMode = 'WASM';
      this.engine = new WasmEngineProxy(
          this.engineId,
          createWasmEngine(this.engineId),
          LoadingManager.getInstance);
    }

    globals.dispatch(Actions.setEngineReady({
      engineId: this.engineId,
      ready: false,
      mode: engineMode,
    }));
    const engineCfg = assertExists(globals.state.engines[this.engineId]);
    let traceStream: TraceStream|undefined;
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
      for (;;) {
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

    const traceTime = await this.engine.getTraceTimeBounds();
    const traceTimeState = {
      startSec: traceTime.start,
      endSec: traceTime.end,
    };
    const actions: DeferredAction[] = [
      Actions.setTraceTime(traceTimeState),
      Actions.navigate({route: '/viewer'}),
    ];

    // We don't know the resolution at this point. However this will be
    // replaced in 50ms so a guess is fine.
    const resolution = (traceTime.end - traceTime.start) / 1000;
    actions.push(Actions.setVisibleTraceTime(
        {...traceTimeState, lastUpdate: Date.now() / 1000, resolution}));

    globals.dispatchMultiple(actions);

    // Make sure the helper views are available before we start adding tracks.
    await this.initialiseHelperViews();

    {
      // When we reload from a permalink don't create extra tracks:
      const {pinnedTracks, tracks} = globals.state;
      if (!pinnedTracks.length && !Object.keys(tracks).length) {
        await this.listTracks();
      }
    }

    await this.listThreads();
    await this.loadTimelineOverview(traceTime);
    globals.dispatch(Actions.sortThreadTracks({}));
    await this.selectFirstHeapProfile();

    return engineMode;
  }

  private async selectFirstHeapProfile() {
    const query = `select * from
    (select distinct(ts) as ts, 'native' as type,
        upid from heap_profile_allocation
        union
        select distinct(graph_sample_ts) as ts, 'graph' as type, upid from
        heap_graph_object) order by ts limit 1`;
    const profile = await assertExists(this.engine).query(query);
    if (profile.numRecords !== 1) return;
    const ts = profile.columns[0].longValues![0];
    const type = profile.columns[1].stringValues![0];
    const upid = profile.columns[2].longValues![0];
    globals.dispatch(Actions.selectHeapProfile({id: 0, upid, ts, type}));
  }

  private async listTracks() {
    this.updateStatus('Loading tracks');
    const engine = assertExists<Engine>(this.engine);
    const actions = await decideTracks(this.engineId, engine);
    globals.dispatchMultiple(actions);
  }

  private async listThreads() {
    this.updateStatus('Reading thread list');
    const sqlQuery = `select utid, tid, pid, thread.name,
        ifnull(
          case when length(process.name) > 0 then process.name else null end,
          thread.name),
        process.cmdline
        from (select * from thread order by upid) as thread
        left join (select * from process order by upid) as process
        using(upid)`;
    const threadRows = await assertExists(this.engine).query(sqlQuery);
    const threads: ThreadDesc[] = [];
    for (let i = 0; i < slowlyCountRows(threadRows); i++) {
      const utid = threadRows.columns[0].longValues![i];
      const tid = threadRows.columns[1].longValues![i];
      const pid = threadRows.columns[2].longValues![i];
      const threadName = threadRows.columns[3].stringValues![i];
      const procName = threadRows.columns[4].stringValues![i];
      const cmdline = threadRows.columns[5].stringValues![i];
      threads.push({utid, tid, threadName, pid, procName, cmdline});
    }  // for (record ...)
    globals.publish('Threads', threads);
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
      const schedRows = await engine.query(
          `select sum(dur)/${stepSec}/1e9, cpu from sched ` +
          `where ts >= ${startNs} and ts < ${endNs} and utid != 0 ` +
          'group by cpu order by cpu');
      const schedData: {[key: string]: QuantizedLoad} = {};
      for (let i = 0; i < slowlyCountRows(schedRows); i++) {
        const load = schedRows.columns[0].doubleValues![i];
        const cpu = schedRows.columns[1].longValues![i];
        schedData[cpu] = {startSec, endSec, load};
        hasSchedOverview = true;
      }  // for (record ...)
      globals.publish('OverviewData', schedData);
    }  // for (step ...)

    if (hasSchedOverview) {
      return;
    }

    // Slices overview.
    const traceStartNs = toNs(traceTime.start);
    const stepSecNs = toNs(stepSec);
    const sliceSummaryQuery = await engine.query(`select
           bucket,
           upid,
           sum(utid_sum) / cast(${stepSecNs} as float) as upid_sum
         from thread
         inner join (
           select
             cast((ts - ${traceStartNs})/${stepSecNs} as int) as bucket,
             sum(dur) as utid_sum,
             utid
           from slice
           inner join thread_track on slice.track_id = thread_track.id
           group by bucket, utid
         ) using(utid)
         group by bucket, upid`);

    const slicesData: {[key: string]: QuantizedLoad[]} = {};
    for (let i = 0; i < slowlyCountRows(sliceSummaryQuery); i++) {
      const bucket = sliceSummaryQuery.columns[0].longValues![i];
      const upid = sliceSummaryQuery.columns[1].longValues![i];
      const load = sliceSummaryQuery.columns[2].doubleValues![i];

      const startSec = traceTime.start + stepSec * bucket;
      const endSec = startSec + stepSec;

      const upidStr = upid.toString();
      let loadArray = slicesData[upidStr];
      if (loadArray === undefined) {
        loadArray = slicesData[upidStr] = [];
      }
      loadArray.push({startSec, endSec, load});
    }
    globals.publish('OverviewData', slicesData);
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
        upid INTEGER
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

    for (const metric
             of ['android_startup',
                 'android_ion',
                 'android_thread_time_in_state',
                 'android_surfaceflinger',
                 'android_batt']) {
      this.updateStatus(`Computing ${metric} metric`);
      try {
        // We don't care about the actual result of metric here as we are just
        // interested in the annotation tracks.
        await engine.computeMetric([metric]);
      } catch (e) {
        if (e instanceof QueryError) {
          globals.publish('MetricError', 'MetricError: ' + e.message);
          continue;
        } else {
          throw e;
        }
      }

      this.updateStatus(`Inserting data for ${metric} metric`);
      try {
        const result = await engine.query(`
        SELECT * FROM ${metric}_event LIMIT 1`);

        const hasSliceName =
            result.columnDescriptors.some(x => x.name === 'slice_name');
        const hasDur = result.columnDescriptors.some(x => x.name === 'dur');
        const hasUpid = result.columnDescriptors.some(x => x.name === 'upid');

        const upidColumnSelect = hasUpid ? 'upid' : '0 AS upid';
        const upidColumnWhere = hasUpid ? 'upid' : '0';
        if (hasSliceName && hasDur) {
          await engine.query(`
            INSERT INTO annotation_slice_track(name, __metric_name, upid)
            SELECT DISTINCT
              track_name,
              '${metric}' as metric_name,
              ${upidColumnSelect}
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

        const hasValue = result.columnDescriptors.some(x => x.name === 'value');
        if (hasValue) {
          const minMax = await engine.query(`
          SELECT MIN(value) as min_value, MAX(value) as max_value
          FROM ${metric}_event
          WHERE ${upidColumnWhere} != 0`);
          const min = minMax.columns[0].longValues![0];
          const max = minMax.columns[1].longValues![0];
          await engine.query(`
          INSERT INTO annotation_counter_track(
            name, __metric_name, min_value, max_value, upid)
          SELECT DISTINCT
            track_name,
            '${metric}' as metric_name,
            CASE ${upidColumnWhere} WHEN 0 THEN NULL ELSE ${min} END,
            CASE ${upidColumnWhere} WHEN 0 THEN NULL ELSE ${max} END,
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
          globals.publish('MetricError', 'MetricError: ' + e.message);
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


