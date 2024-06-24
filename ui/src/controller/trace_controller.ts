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

import {BigintMath} from '../base/bigint_math';
import {assertExists, assertTrue} from '../base/logging';
import {Duration, duration, Span, time, Time, TimeSpan} from '../base/time';
import {Actions, DeferredAction} from '../common/actions';
import {cacheTrace} from '../common/cache_manager';
import {
  HighPrecisionTime,
  HighPrecisionTimeSpan,
} from '../common/high_precision_time';
import {
  getEnabledMetatracingCategories,
  isMetatracingEnabled,
} from '../common/metatracing';
import {pluginManager} from '../common/plugins';
import {EngineMode, PendingDeeplinkState, ProfileType} from '../common/state';
import {featureFlags, Flag, PERF_SAMPLE_FLAG} from '../core/feature_flags';
import {
  defaultTraceContext,
  globals,
  QuantizedLoad,
  ThreadDesc,
  TraceContext,
} from '../frontend/globals';
import {
  clearOverviewData,
  publishHasFtrace,
  publishMetricError,
  publishOverviewData,
  publishThreads,
  publishTraceContext,
} from '../frontend/publish';
import {addQueryResultsTab} from '../frontend/query_result_tab';
import {Router} from '../frontend/router';
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
import {
  resetEngineWorker,
  WasmEngineProxy,
} from '../trace_processor/wasm_engine_proxy';
import {showModal} from '../widgets/modal';

import {CounterAggregationController} from './aggregation/counter_aggregation_controller';
import {CpuAggregationController} from './aggregation/cpu_aggregation_controller';
import {CpuByProcessAggregationController} from './aggregation/cpu_by_process_aggregation_controller';
import {FrameAggregationController} from './aggregation/frame_aggregation_controller';
import {SliceAggregationController} from './aggregation/slice_aggregation_controller';
import {ThreadAggregationController} from './aggregation/thread_aggregation_controller';
import {Child, Children, Controller} from './controller';
import {
  CpuProfileController,
  CpuProfileControllerArgs,
} from './cpu_profile_controller';
import {
  FlowEventsController,
  FlowEventsControllerArgs,
} from './flow_events_controller';
import {LoadingManager} from './loading_manager';
import {
  PIVOT_TABLE_REDUX_FLAG,
  PivotTableController,
} from './pivot_table_controller';
import {SearchController} from './search_controller';
import {
  SelectionController,
  SelectionControllerArgs,
} from './selection_controller';
import {TraceErrorController} from './trace_error_controller';
import {
  TraceBufferStream,
  TraceFileStream,
  TraceHttpStream,
  TraceStream,
} from '../core/trace_stream';
import {decideTracks} from './track_decider';
import {profileType} from '../frontend/legacy_flamegraph_panel';
import {FlamegraphCache} from '../core/flamegraph_cache';
import {
  deserializeAppStatePhase1,
  deserializeAppStatePhase2,
} from '../common/state_serialization';

type States = 'init' | 'loading_trace' | 'ready';

const METRICS = [
  'android_ion',
  'android_lmk',
  'android_dma_heap',
  'android_surfaceflinger',
  'android_batt',
  'android_other_traces',
  'chrome_dropped_frames',
  // TODO(289365196): Reenable:
  // 'chrome_long_latency',
  'trace_metadata',
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

// A local storage key where the indication that JSON warning has been shown is
// stored.
const SHOWN_JSON_WARNING_KEY = 'shownJsonWarning';

function showJsonWarning() {
  showModal({
    title: 'Warning',
    content: m(
      'div',
      m(
        'span',
        'Perfetto UI features are limited for JSON traces. ',
        'We recommend recording ',
        m(
          'a',
          {href: 'https://perfetto.dev/docs/quickstart/chrome-tracing'},
          'proto-format traces',
        ),
        ' from Chrome.',
      ),
      m('br'),
    ),
    buttons: [],
  });
}

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
  private readonly engineId: string;
  private engine?: EngineBase;

  constructor(engineId: string) {
    super('init');
    this.engineId = engineId;
  }

  run() {
    const engineCfg = assertExists(globals.state.engine);
    switch (this.state) {
      case 'init':
        this.loadTrace()
          .then((mode) => {
            globals.dispatch(
              Actions.setEngineReady({
                engineId: this.engineId,
                ready: true,
                mode,
              }),
            );
          })
          .catch((err) => {
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

        const selectionArgs: SelectionControllerArgs = {engine};
        childControllers.push(
          Child('selection', SelectionController, selectionArgs),
        );

        const flowEventsArgs: FlowEventsControllerArgs = {engine};
        childControllers.push(
          Child('flowEvents', FlowEventsController, flowEventsArgs),
        );

        const cpuProfileArgs: CpuProfileControllerArgs = {engine};
        childControllers.push(
          Child('cpuProfile', CpuProfileController, cpuProfileArgs),
        );

        childControllers.push(
          Child('cpu_aggregation', CpuAggregationController, {
            engine,
            kind: 'cpu_aggregation',
          }),
        );
        childControllers.push(
          Child('thread_aggregation', ThreadAggregationController, {
            engine,
            kind: 'thread_state_aggregation',
          }),
        );
        childControllers.push(
          Child('cpu_process_aggregation', CpuByProcessAggregationController, {
            engine,
            kind: 'cpu_by_process_aggregation',
          }),
        );
        if (!PIVOT_TABLE_REDUX_FLAG.get()) {
          // Pivot table is supposed to handle the use cases the slice
          // aggregation panel is used right now. When a flag to use pivot
          // tables is enabled, do not add slice aggregation controller.
          childControllers.push(
            Child('slice_aggregation', SliceAggregationController, {
              engine,
              kind: 'slice_aggregation',
            }),
          );
        }
        childControllers.push(
          Child('counter_aggregation', CounterAggregationController, {
            engine,
            kind: 'counter_aggregation',
          }),
        );
        childControllers.push(
          Child('frame_aggregation', FrameAggregationController, {
            engine,
            kind: 'frame_aggregation',
          }),
        );
        childControllers.push(
          Child('search', SearchController, {
            engine,
            app: globals,
          }),
        );
        childControllers.push(
          Child('pivot_table', PivotTableController, {engine}),
        );

        childControllers.push(
          Child('traceError', TraceErrorController, {engine}),
        );

        return childControllers;

      default:
        throw new Error(`unknown state ${this.state}`);
    }
    return;
  }

  onDestroy() {
    pluginManager.onTraceClose();
    globals.engines.delete(this.engineId);

    // Invalidate the flamegraph cache.
    // TODO(stevegolton): migrate this to the new system when it's ready.
    globals.areaFlamegraphCache = new FlamegraphCache('area');
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
          Actions.setEngineFailed({mode: 'HTTP_RPC', failure: `${err}`}),
        );
        throw err;
      };
    } else {
      console.log('Opening trace using built-in WASM engine');
      engineMode = 'WASM';
      const enginePort = resetEngineWorker();
      engine = new WasmEngineProxy(
        this.engineId,
        enginePort,
        LoadingManager.getInstance,
      );
      engine.resetTraceProcessor({
        cropTrackEvents: CROP_TRACK_EVENTS_FLAG.get(),
        ingestFtraceInRawTable: INGEST_FTRACE_IN_RAW_TABLE_FLAG.get(),
        analyzeTraceProtoContent: ANALYZE_TRACE_PROTO_CONTENT_FLAG.get(),
        ftraceDropUntilAllCpusValid: FTRACE_DROP_UNTIL_FLAG.get(),
      });
    }
    this.engine = engine;

    if (isMetatracingEnabled()) {
      this.engine.enableMetatrace(
        assertExists(getEnabledMetatracingCategories()),
      );
    }

    globals.engines.set(this.engineId, engine);
    globals.dispatch(
      Actions.setEngineReady({
        engineId: this.engineId,
        ready: false,
        mode: engineMode,
      }),
    );
    const engineCfg = assertExists(globals.state.engine);
    assertTrue(engineCfg.id === this.engineId);
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
      for (;;) {
        const res = await traceStream.readChunk();
        await this.engine.parse(res.data);
        const elapsed = (performance.now() - tStart) / 1000;
        let status = 'Loading trace ';
        if (res.bytesTotal > 0) {
          const progress = Math.round((res.bytesRead / res.bytesTotal) * 100);
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

    const traceDetails = await getTraceTimeDetails(this.engine);
    publishTraceContext(traceDetails);

    const shownJsonWarning =
      window.localStorage.getItem(SHOWN_JSON_WARNING_KEY) !== null;

    // Show warning if the trace is in JSON format.
    const query = `select str_value from metadata where name = 'trace_type'`;
    const result = await assertExists(this.engine).query(query);
    const traceType = result.firstRow({str_value: STR}).str_value;
    const isJsonTrace = traceType == 'json';
    if (!shownJsonWarning) {
      // When in embedded mode, the host app will control which trace format
      // it passes to Perfetto, so we don't need to show this warning.
      if (isJsonTrace && !globals.embeddedMode) {
        showJsonWarning();
        // Save that the warning has been shown. Value is irrelevant since only
        // the presence of key is going to be checked.
        window.localStorage.setItem(SHOWN_JSON_WARNING_KEY, 'true');
      }
    }

    const emptyOmniboxState = {
      omnibox: '',
      mode: globals.state.omniboxState.mode || 'SEARCH',
    };

    const actions: DeferredAction[] = [
      Actions.setOmnibox(emptyOmniboxState),
      Actions.setTraceUuid({traceUuid}),
    ];

    const visibleTimeSpan = await computeVisibleTime(
      traceDetails.start,
      traceDetails.end,
      isJsonTrace,
      this.engine,
    );
    // We don't know the resolution at this point. However this will be
    // replaced in 50ms so a guess is fine.
    const resolution = visibleTimeSpan.duration.divide(1000).toTime();
    actions.push(
      Actions.setVisibleTraceTime({
        start: visibleTimeSpan.start.toTime(),
        end: visibleTimeSpan.end.toTime(),
        lastUpdate: Date.now() / 1000,
        resolution: BigintMath.max(resolution, 1n),
      }),
    );

    globals.dispatchMultiple(actions);
    Router.navigate(`#!/viewer?local_cache_key=${traceUuid}`);

    // Make sure the helper views are available before we start adding tracks.
    await this.initialiseHelperViews();
    await this.includeSummaryTables();

    await defineMaxLayoutDepthSqlFunction(engine);

    if (globals.restoreAppStateAfterTraceLoad) {
      deserializeAppStatePhase1(globals.restoreAppStateAfterTraceLoad);
    }

    await pluginManager.onTraceLoad(engine, (id) => {
      this.updateStatus(`Running plugin: ${id}`);
    });

    {
      // When we reload from a permalink don't create extra tracks:
      const {pinnedTracks, tracks} = globals.state;
      if (!pinnedTracks.length && !Object.keys(tracks).length) {
        await this.listTracks();
      }
    }

    this.decideTabs();

    await this.listThreads();
    await this.loadTimelineOverview(
      new TimeSpan(traceDetails.start, traceDetails.end),
    );

    {
      // Check if we have any ftrace events at all
      const query = `
        select
          *
        from ftrace_event
        limit 1`;

      const res = await engine.query(query);
      publishHasFtrace(res.numRows() > 0);
    }

    globals.dispatch(Actions.sortThreadTracks({}));
    globals.dispatch(Actions.maybeExpandOnlyTrackGroup({}));

    await this.selectFirstHeapProfile();
    if (PERF_SAMPLE_FLAG.get()) {
      await this.selectPerfSample(traceDetails);
    }

    const pendingDeeplink = globals.state.pendingDeeplink;
    if (pendingDeeplink !== undefined) {
      globals.dispatch(Actions.clearPendingDeeplink({}));
      await this.selectPendingDeeplink(pendingDeeplink);
      if (
        pendingDeeplink.visStart !== undefined &&
        pendingDeeplink.visEnd !== undefined
      ) {
        this.zoomPendingDeeplink(
          pendingDeeplink.visStart,
          pendingDeeplink.visEnd,
        );
      }
      if (pendingDeeplink.query !== undefined) {
        addQueryResultsTab({
          query: pendingDeeplink.query,
          title: 'Deeplink Query',
        });
      }
    }

    globals.dispatch(Actions.maybeExpandOnlyTrackGroup({}));

    // Trace Processor doesn't support the reliable range feature for JSON
    // traces.
    if (!isJsonTrace && ENABLE_CHROME_RELIABLE_RANGE_ANNOTATION_FLAG.get()) {
      const reliableRangeStart = await computeTraceReliableRangeStart(engine);
      if (reliableRangeStart > 0) {
        globals.dispatch(
          Actions.addNote({
            timestamp: reliableRangeStart,
            color: '#ff0000',
            text: 'Reliable Range Start',
          }),
        );
      }
    }

    if (globals.restoreAppStateAfterTraceLoad) {
      // Wait that plugins have completed their actions and then proceed with
      // the final phase of app state restore.
      // TODO(primiano): this can probably be removed once we refactor tracks
      // to be URI based and can deal with non-existing URIs.
      deserializeAppStatePhase2(globals.restoreAppStateAfterTraceLoad);
      globals.restoreAppStateAfterTraceLoad = undefined;
    }
    return engineMode;
  }

  private async selectPerfSample(traceTime: {start: time; end: time}) {
    const query = `select upid
        from perf_sample
        join thread using (utid)
        where callsite_id is not null
        order by ts desc limit 1`;
    const profile = await assertExists(this.engine).query(query);
    if (profile.numRows() !== 1) return;
    const row = profile.firstRow({upid: NUM});
    const upid = row.upid;
    const leftTs = traceTime.start;
    const rightTs = traceTime.end;
    globals.dispatch(
      Actions.selectPerfSamples({
        id: 0,
        upid,
        leftTs,
        rightTs,
        type: ProfileType.PERF_SAMPLE,
      }),
    );
  }

  private async selectFirstHeapProfile() {
    const query = `select * from (
      select
        min(ts) AS ts,
        'heap_profile:' || group_concat(distinct heap_name) AS type,
        upid
      from heap_profile_allocation
      group by upid
      union
      select distinct graph_sample_ts as ts, 'graph' as type, upid
      from heap_graph_object)
      order by ts limit 1`;
    const profile = await assertExists(this.engine).query(query);
    if (profile.numRows() !== 1) return;
    const row = profile.firstRow({ts: LONG, type: STR, upid: NUM});
    const ts = Time.fromRaw(row.ts);
    let profType = row.type;
    if (profType == 'heap_profile:libc.malloc,com.android.art') {
      profType = 'heap_profile:com.android.art,libc.malloc';
    }
    const type = profileType(profType);
    const upid = row.upid;
    globals.dispatch(Actions.selectHeapProfile({id: 0, upid, ts, type}));
  }

  private async selectPendingDeeplink(link: PendingDeeplinkState) {
    const conditions = [];
    const {ts, dur} = link;

    if (ts !== undefined) {
      conditions.push(`ts = ${ts}`);
    }
    if (dur !== undefined) {
      conditions.push(`dur = ${dur}`);
    }

    if (conditions.length === 0) {
      return;
    }

    const query = `
      select
        id,
        track_id as traceProcessorTrackId,
        type
      from slice
      where ${conditions.join(' and ')}
    ;`;

    const result = await assertExists(this.engine).query(query);
    if (result.numRows() > 0) {
      const row = result.firstRow({
        id: NUM,
        traceProcessorTrackId: NUM,
        type: STR,
      });

      const id = row.traceProcessorTrackId;
      const trackKey = globals.trackManager.trackKeyByTrackId.get(id);
      if (trackKey === undefined) {
        return;
      }
      globals.setLegacySelection(
        {
          kind: 'SLICE',
          id: row.id,
          trackKey,
          table: 'slice',
        },
        {
          clearSearch: true,
          pendingScrollId: row.id,
          switchToCurrentSelectionTab: false,
        },
      );
    }
  }

  private async listTracks() {
    this.updateStatus('Loading tracks');
    const engine = assertExists(this.engine);
    const actions = await decideTracks(engine);
    globals.dispatchMultiple(actions);
  }

  // Show the list of default tabs, but don't make them active!
  private decideTabs() {
    for (const tabUri of globals.tabManager.defaultTabs) {
      globals.dispatch(Actions.showTab({uri: tabUri}));
    }
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
      threads.push({utid, tid, threadName, pid, procName, cmdline});
    }
    publishThreads(threads);
  }

  private async loadTimelineOverview(trace: Span<time, duration>) {
    clearOverviewData();
    const engine = assertExists<Engine>(this.engine);
    const stepSize = Duration.max(1n, trace.duration / 100n);
    const hasSchedSql = 'select ts from sched limit 1';
    const hasSchedOverview = (await engine.query(hasSchedSql)).numRows() > 0;
    if (hasSchedOverview) {
      const stepPromises = [];
      for (
        let start = trace.start;
        start < trace.end;
        start = Time.add(start, stepSize)
      ) {
        const progress = start - trace.start;
        const ratio = Number(progress) / Number(trace.duration);
        this.updateStatus('Loading overview ' + `${Math.round(ratio * 100)}%`);
        const end = Time.add(start, stepSize);
        // The (async() => {})() queues all the 100 async promises in one batch.
        // Without that, we would wait for each step to be rendered before
        // kicking off the next one. That would interleave an animation frame
        // between each step, slowing down significantly the overall process.
        stepPromises.push(
          (async () => {
            const schedResult = await engine.query(
              `select cast(sum(dur) as float)/${stepSize} as load, cpu from sched ` +
                `where ts >= ${start} and ts < ${end} and utid != 0 ` +
                'group by cpu order by cpu',
            );
            const schedData: {[key: string]: QuantizedLoad} = {};
            const it = schedResult.iter({load: NUM, cpu: NUM});
            for (; it.valid(); it.next()) {
              const load = it.load;
              const cpu = it.cpu;
              schedData[cpu] = {start, end, load};
            }
            publishOverviewData(schedData);
          })(),
        );
      } // for(start = ...)
      await Promise.all(stepPromises);
      return;
    } // if (hasSchedOverview)

    // Slices overview.
    const sliceResult = await engine.query(`select
           bucket,
           upid,
           ifnull(sum(utid_sum) / cast(${stepSize} as float), 0) as load
         from thread
         inner join (
           select
             ifnull(cast((ts - ${trace.start})/${stepSize} as int), 0) as bucket,
             sum(dur) as utid_sum,
             utid
           from slice
           inner join thread_track on slice.track_id = thread_track.id
           group by bucket, utid
         ) using(utid)
         where upid is not null
         group by bucket, upid`);

    const slicesData: {[key: string]: QuantizedLoad[]} = {};
    const it = sliceResult.iter({bucket: LONG, upid: NUM, load: NUM});
    for (; it.valid(); it.next()) {
      const bucket = it.bucket;
      const upid = it.upid;
      const load = it.load;

      const start = Time.add(trace.start, stepSize * bucket);
      const end = Time.add(start, stepSize);

      const upidStr = upid.toString();
      let loadArray = slicesData[upidStr];
      if (loadArray === undefined) {
        loadArray = slicesData[upidStr] = [];
      }
      loadArray.push({start, end, load});
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
    const traceUuid = result.firstRow({uuid: STR}).uuid;
    const engineConfig = assertExists(globals.state.engine);
    assertTrue(engineConfig.id === this.engineId);
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
    const engine = assertExists(this.engine);

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
        id BIGINT,
        track_id INT,
        ts BIGINT,
        value DOUBLE,
        PRIMARY KEY (track_id, ts)
      ) WITHOUT ROWID;
    `);
    this.updateStatus('Creating annotation slice table');
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

      this.updateStatus(`Computing ${metric} metric`);
      try {
        // We don't care about the actual result of metric here as we are just
        // interested in the annotation tracks.
        await engine.computeMetric([metric], 'proto');
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
          publishMetricError('MetricError: ' + e.message);
        } else {
          throw e;
        }
      }
    }
  }

  async includeSummaryTables() {
    const engine = assertExists<Engine>(this.engine);

    this.updateStatus('Creating slice summaries');
    await engine.query(`include perfetto module viz.summary.slices;`);

    this.updateStatus('Creating counter summaries');
    await engine.query(`include perfetto module viz.summary.counters;`);

    this.updateStatus('Creating thread summaries');
    await engine.query(`include perfetto module viz.summary.threads;`);

    this.updateStatus('Creating processes summaries');
    await engine.query(`include perfetto module viz.summary.processes;`);

    this.updateStatus('Creating track summaries');
    await engine.query(`include perfetto module viz.summary.tracks;`);
  }

  private updateStatus(msg: string): void {
    globals.dispatch(
      Actions.updateStatus({
        msg,
        timestamp: Date.now() / 1000,
      }),
    );
  }

  private zoomPendingDeeplink(visStart: string, visEnd: string) {
    const visualStart = Time.fromRaw(BigInt(visStart));
    const visualEnd = Time.fromRaw(BigInt(visEnd));
    const traceTime = globals.stateTraceTimeTP();

    if (
      !(
        visualStart < visualEnd &&
        traceTime.start <= visualStart &&
        visualEnd <= traceTime.end
      )
    ) {
      return;
    }

    const res = (visualEnd - visualStart) / 1000n;

    globals.dispatch(
      Actions.setVisibleTraceTime({
        start: visualStart,
        end: visualEnd,
        resolution: BigintMath.max(res, 1n),
        lastUpdate: Date.now() / 1000,
      }),
    );
  }
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
): Promise<Span<HighPrecisionTime>> {
  // if we have non-default visible state, update the visible time to it
  const previousVisibleState = globals.stateVisibleTime();
  const defaultTraceSpan = new TimeSpan(
    defaultTraceContext.start,
    defaultTraceContext.end,
  );
  if (
    !(
      previousVisibleState.start === defaultTraceSpan.start &&
      previousVisibleState.end === defaultTraceSpan.end
    ) &&
    previousVisibleState.start >= traceStart &&
    previousVisibleState.end <= traceEnd
  ) {
    return HighPrecisionTimeSpan.fromTime(
      previousVisibleState.start,
      previousVisibleState.end,
    );
  }

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
  return HighPrecisionTimeSpan.fromTime(visibleStart, visibleEnd);
}

async function getTraceTimeDetails(engine: Engine): Promise<TraceContext> {
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

  return {
    ...traceTime,
    realtimeOffset,
    utcOffset,
    traceTzOffset,
    cpus: await getCpus(engine),
    gpuCount: await getNumberOfGpus(engine),
  };
}

async function getTraceTimeBounds(
  engine: Engine,
): Promise<Span<time, duration>> {
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

async function getTracingMetadataTimeBounds(
  engine: Engine,
): Promise<Span<time, duration>> {
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
