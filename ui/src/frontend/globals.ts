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

import {assertExists, assertUnreachable} from '../base/logging';
import {createStore, Store} from '../base/store';
import {duration, Time, time, TimeSpan} from '../base/time';
import {Actions, DeferredAction} from '../common/actions';
import {AggregateData} from '../common/aggregation_data';
import {Args} from '../common/arg_types';
import {CommandManager} from '../common/commands';
import {
  ConversionJobName,
  ConversionJobStatus,
} from '../common/conversion_jobs';
import {createEmptyState} from '../common/empty_state';
import {MetricResult} from '../common/metric_data';
import {CurrentSearchResults} from '../common/search_data';
import {EngineConfig, State, getLegacySelection} from '../common/state';
import {TabManager} from '../common/tab_registry';
import {TimestampFormat, timestampFormat} from '../core/timestamp_format';
import {TrackManager} from '../common/track_cache';
import {setPerfHooks} from '../core/perf';
import {raf} from '../core/raf_scheduler';
import {ServiceWorkerController} from './service_worker_controller';
import {Engine, EngineBase} from '../trace_processor/engine';
import {HttpRpcState} from '../trace_processor/http_rpc_engine';
import {Analytics, initAnalytics} from './analytics';
import {Timeline} from './timeline';
import {SliceSqlId} from '../trace_processor/sql_utils/core_types';
import {SelectionManager, LegacySelection} from '../core/selection_manager';
import {Optional, exists} from '../base/utils';
import {OmniboxManager} from './omnibox_manager';
import {CallsiteInfo} from '../common/legacy_flamegraph_util';
import {LegacyFlamegraphCache} from '../core/legacy_flamegraph_cache';
import {SerializedAppState} from '../common/state_serialization_schema';
import {getServingRoot} from '../base/http_utils';
import {
  createSearchOverviewTrack,
  SearchOverviewTrack,
} from './search_overview_track';
import {AppContext} from './app_context';
import {TraceContext} from './trace_context';
import {Registry} from '../base/registry';
import {SidebarMenuItem} from '../public';

const INSTANT_FOCUS_DURATION = 1n;
const INCOMPLETE_SLICE_DURATION = 30_000n;

type DispatchMultiple = (actions: DeferredAction[]) => void;
type TrackDataStore = Map<string, {}>;
type QueryResultsStore = Map<string, {} | undefined>;
type AggregateDataStore = Map<string, AggregateData>;
type Description = Map<string, string>;

export interface SliceDetails {
  ts?: time;
  absTime?: string;
  dur?: duration;
  threadTs?: time;
  threadDur?: duration;
  priority?: number;
  endState?: string | null;
  cpu?: number;
  id?: number;
  threadStateId?: number;
  utid?: number;
  wakeupTs?: time;
  wakerUtid?: number;
  wakerCpu?: number;
  category?: string;
  name?: string;
  tid?: number;
  threadName?: string;
  pid?: number;
  processName?: string;
  uid?: number;
  packageName?: string;
  versionCode?: number;
  args?: Args;
  description?: Description;
}

export interface FlowPoint {
  trackId: number;

  sliceName: string;
  sliceCategory: string;
  sliceId: SliceSqlId;
  sliceStartTs: time;
  sliceEndTs: time;
  // Thread and process info. Only set in sliceSelected not in areaSelected as
  // the latter doesn't display per-flow info and it'd be a waste to join
  // additional tables for undisplayed info in that case. Nothing precludes
  // adding this in a future iteration however.
  threadName: string;
  processName: string;

  depth: number;

  // TODO(altimin): Ideally we should have a generic mechanism for allowing to
  // customise the name here, but for now we are hardcording a few
  // Chrome-specific bits in the query here.
  sliceChromeCustomName?: string;
}

export interface Flow {
  id: number;

  begin: FlowPoint;
  end: FlowPoint;
  dur: duration;

  // Whether this flow connects a slice with its descendant.
  flowToDescendant: boolean;

  category?: string;
  name?: string;
}

export interface ThreadStateDetails {
  ts?: time;
  dur?: duration;
}

export interface CpuProfileDetails {
  id?: number;
  ts?: time;
  utid?: number;
  stack?: CallsiteInfo[];
}

export interface QuantizedLoad {
  start: time;
  end: time;
  load: number;
}
type OverviewStore = Map<string, QuantizedLoad[]>;

export interface ThreadDesc {
  utid: number;
  tid: number;
  threadName: string;
  pid?: number;
  procName?: string;
  cmdline?: string;
}
type ThreadMap = Map<number, ThreadDesc>;

// Options for globals.makeSelection().
export interface MakeSelectionOpts {
  // Whether to switch to the current selection tab or not. Default = true.
  switchToCurrentSelectionTab?: boolean;

  // Whether to cancel the current search selection. Default = true.
  clearSearch?: boolean;
}

// All of these control additional things we can do when doing a
// selection.
export interface LegacySelectionArgs {
  clearSearch: boolean;
  switchToCurrentSelectionTab: boolean;
  pendingScrollId: number | undefined;
}

export const defaultTraceContext: TraceContext = {
  traceTitle: '',
  traceUrl: '',
  start: Time.ZERO,
  end: Time.fromSeconds(10),
  realtimeOffset: Time.ZERO,
  utcOffset: Time.ZERO,
  traceTzOffset: Time.ZERO,
  cpus: [],
  gpuCount: 0,
};

/**
 * Global accessors for state/dispatch in the frontend.
 */
class Globals implements AppContext {
  readonly root = getServingRoot();

  private _testing = false;
  private _dispatchMultiple?: DispatchMultiple = undefined;
  private _store = createStore<State>(createEmptyState());
  private _timeline?: Timeline = undefined;
  private _serviceWorkerController?: ServiceWorkerController = undefined;
  private _logging?: Analytics = undefined;
  private _isInternalUser: boolean | undefined = undefined;

  // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.
  private _trackDataStore?: TrackDataStore = undefined;
  private _queryResults?: QueryResultsStore = undefined;
  private _overviewStore?: OverviewStore = undefined;
  private _aggregateDataStore?: AggregateDataStore = undefined;
  private _threadMap?: ThreadMap = undefined;
  private _sliceDetails?: SliceDetails = undefined;
  private _threadStateDetails?: ThreadStateDetails = undefined;
  private _connectedFlows?: Flow[] = undefined;
  private _selectedFlows?: Flow[] = undefined;
  private _visibleFlowCategories?: Map<string, boolean> = undefined;
  private _cpuProfileDetails?: CpuProfileDetails = undefined;
  private _numQueriesQueued = 0;
  private _bufferUsage?: number = undefined;
  private _recordingLog?: string = undefined;
  private _traceErrors?: number = undefined;
  private _metricError?: string = undefined;
  private _metricResult?: MetricResult = undefined;
  private _jobStatus?: Map<ConversionJobName, ConversionJobStatus> = undefined;
  private _embeddedMode?: boolean = undefined;
  private _hideSidebar?: boolean = undefined;
  private _cmdManager = new CommandManager();
  private _tabManager = new TabManager();
  private _trackManager = new TrackManager(this._store);
  private _selectionManager = new SelectionManager(this._store);
  private _hasFtrace: boolean = false;
  private _searchOverviewTrack?: SearchOverviewTrack;

  omnibox = new OmniboxManager();
  areaFlamegraphCache = new LegacyFlamegraphCache('area');

  scrollToTrackKey?: string | number;
  httpRpcState: HttpRpcState = {connected: false};
  showPanningHint = false;
  permalinkHash?: string;
  showTraceErrorPopup = true;

  traceContext = defaultTraceContext;
  currentTraceName = '';

  readonly sidebarMenuItems = new Registry<SidebarMenuItem>((m) => m.commandId);

  // This is the app's equivalent of a plugin's onTraceLoad() function.
  // TODO(stevegolton): Eventually initialization that should be done on trace
  // load should be moved into here, and then we can remove TraceController
  // entirely
  async onTraceLoad(engine: Engine, traceCtx: TraceContext): Promise<void> {
    this.traceContext = traceCtx;

    const {start, end} = traceCtx;
    const traceSpan = new TimeSpan(start, end);
    this._timeline = new Timeline(this._store, traceSpan);

    // TODO(stevegolton): Even though createSearchOverviewTrack() returns a
    // disposable, we completely ignore it as we assume the dispose action
    // includes just dropping some tables, and seeing as this object will live
    // for the duration of the trace/engine, there's no need to drop anything as
    // the tables will be dropped along with the trace anyway.
    //
    // Note that this is no worse than a lot of the rest of the app where tables
    // are created with no way to drop them.
    //
    // Once we fix the story around loading new traces, we should tidy this up.
    // We could for example have a matching globals.onTraceUnload() that
    // performs any tear-down before the old engine is dropped. This might seem
    // pointless, but it could at least block until any currently running update
    // cycles complete, to avoid leaving promises open on old engines that will
    // never resolve.
    //
    // Alternatively we could decide that we don't want to support switching
    // traces at all, in which case we can ignore tear down entirely.
    this._searchOverviewTrack = await createSearchOverviewTrack(engine, this);
  }

  // Used for permalink load by trace_controller.ts.
  restoreAppStateAfterTraceLoad?: SerializedAppState;

  // TODO(hjd): Remove once we no longer need to update UUID on redraw.
  private _publishRedraw?: () => void = undefined;

  private _currentSearchResults: CurrentSearchResults = {
    eventIds: new Float64Array(0),
    tses: new BigInt64Array(0),
    utids: new Float64Array(0),
    trackKeys: [],
    sources: [],
    totalResults: 0,
  };

  engines = new Map<string, EngineBase>();

  constructor() {
    const {start, end} = defaultTraceContext;
    this._timeline = new Timeline(this._store, new TimeSpan(start, end));
  }

  initialize(dispatchMultiple: DispatchMultiple) {
    this._dispatchMultiple = dispatchMultiple;

    setPerfHooks(
      () => this.state.perfDebug,
      () => this.dispatch(Actions.togglePerfDebug({})),
    );

    this._serviceWorkerController = new ServiceWorkerController(
      getServingRoot(),
    );
    this._testing =
      /* eslint-disable @typescript-eslint/strict-boolean-expressions */
      self.location && self.location.search.indexOf('testing=1') >= 0;
    /* eslint-enable */
    this._logging = initAnalytics();

    // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.
    this._trackDataStore = new Map<string, {}>();
    this._queryResults = new Map<string, {}>();
    this._overviewStore = new Map<string, QuantizedLoad[]>();
    this._aggregateDataStore = new Map<string, AggregateData>();
    this._threadMap = new Map<number, ThreadDesc>();
    this._sliceDetails = {};
    this._connectedFlows = [];
    this._selectedFlows = [];
    this._visibleFlowCategories = new Map<string, boolean>();
    this._threadStateDetails = {};
    this._cpuProfileDetails = {};
    this.engines.clear();
    this._selectionManager.clear();
  }

  // Only initialises the store - useful for testing.
  initStore(initialState: State) {
    this._store = createStore(initialState);
  }

  get publishRedraw(): () => void {
    return this._publishRedraw || (() => {});
  }

  set publishRedraw(f: () => void) {
    this._publishRedraw = f;
  }

  get state(): State {
    return assertExists(this._store).state;
  }

  get store(): Store<State> {
    return assertExists(this._store);
  }

  dispatch(action: DeferredAction) {
    this.dispatchMultiple([action]);
  }

  dispatchMultiple(actions: DeferredAction[]) {
    assertExists(this._dispatchMultiple)(actions);
  }

  get timeline() {
    return assertExists(this._timeline);
  }

  get logging() {
    return assertExists(this._logging);
  }

  get serviceWorkerController() {
    return assertExists(this._serviceWorkerController);
  }

  // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.
  get overviewStore(): OverviewStore {
    return assertExists(this._overviewStore);
  }

  get trackDataStore(): TrackDataStore {
    return assertExists(this._trackDataStore);
  }

  get queryResults(): QueryResultsStore {
    return assertExists(this._queryResults);
  }

  get threads() {
    return assertExists(this._threadMap);
  }

  get sliceDetails() {
    return assertExists(this._sliceDetails);
  }

  set sliceDetails(click: SliceDetails) {
    this._sliceDetails = assertExists(click);
  }

  get threadStateDetails() {
    return assertExists(this._threadStateDetails);
  }

  set threadStateDetails(click: ThreadStateDetails) {
    this._threadStateDetails = assertExists(click);
  }

  get connectedFlows() {
    return assertExists(this._connectedFlows);
  }

  set connectedFlows(connectedFlows: Flow[]) {
    this._connectedFlows = assertExists(connectedFlows);
  }

  get selectedFlows() {
    return assertExists(this._selectedFlows);
  }

  set selectedFlows(selectedFlows: Flow[]) {
    this._selectedFlows = assertExists(selectedFlows);
  }

  get visibleFlowCategories() {
    return assertExists(this._visibleFlowCategories);
  }

  set visibleFlowCategories(visibleFlowCategories: Map<string, boolean>) {
    this._visibleFlowCategories = assertExists(visibleFlowCategories);
  }

  get aggregateDataStore(): AggregateDataStore {
    return assertExists(this._aggregateDataStore);
  }

  get traceErrors() {
    return this._traceErrors;
  }

  setTraceErrors(arg: number) {
    this._traceErrors = arg;
  }

  get metricError() {
    return this._metricError;
  }

  setMetricError(arg: string) {
    this._metricError = arg;
  }

  get metricResult() {
    return this._metricResult;
  }

  setMetricResult(result: MetricResult) {
    this._metricResult = result;
  }

  get cpuProfileDetails() {
    return assertExists(this._cpuProfileDetails);
  }

  set cpuProfileDetails(click: CpuProfileDetails) {
    this._cpuProfileDetails = assertExists(click);
  }

  set numQueuedQueries(value: number) {
    this._numQueriesQueued = value;
  }

  get numQueuedQueries() {
    return this._numQueriesQueued;
  }

  get bufferUsage() {
    return this._bufferUsage;
  }

  get recordingLog() {
    return this._recordingLog;
  }

  get currentSearchResults() {
    return this._currentSearchResults;
  }

  set currentSearchResults(results: CurrentSearchResults) {
    this._currentSearchResults = results;
  }

  set hasFtrace(value: boolean) {
    this._hasFtrace = value;
  }

  get hasFtrace(): boolean {
    return this._hasFtrace;
  }

  get searchOverviewTrack() {
    return this._searchOverviewTrack;
  }

  getConversionJobStatus(name: ConversionJobName): ConversionJobStatus {
    return this.getJobStatusMap().get(name) ?? ConversionJobStatus.NotRunning;
  }

  setConversionJobStatus(name: ConversionJobName, status: ConversionJobStatus) {
    const map = this.getJobStatusMap();
    if (status === ConversionJobStatus.NotRunning) {
      map.delete(name);
    } else {
      map.set(name, status);
    }
  }

  private getJobStatusMap(): Map<ConversionJobName, ConversionJobStatus> {
    if (!this._jobStatus) {
      this._jobStatus = new Map();
    }
    return this._jobStatus;
  }

  get embeddedMode(): boolean {
    return !!this._embeddedMode;
  }

  set embeddedMode(value: boolean) {
    this._embeddedMode = value;
  }

  get hideSidebar(): boolean {
    return !!this._hideSidebar;
  }

  set hideSidebar(value: boolean) {
    this._hideSidebar = value;
  }

  setBufferUsage(bufferUsage: number) {
    this._bufferUsage = bufferUsage;
  }

  setTrackData(id: string, data: {}) {
    this.trackDataStore.set(id, data);
  }

  setRecordingLog(recordingLog: string) {
    this._recordingLog = recordingLog;
  }

  setAggregateData(kind: string, data: AggregateData) {
    this.aggregateDataStore.set(kind, data);
  }

  getCurrentEngine(): EngineConfig | undefined {
    return this.state.engine;
  }

  makeSelection(action: DeferredAction<{}>, opts: MakeSelectionOpts = {}) {
    const {switchToCurrentSelectionTab = true, clearSearch = true} = opts;
    const currentSelectionTabUri = 'current_selection';

    // A new selection should cancel the current search selection.
    clearSearch && globals.dispatch(Actions.setSearchIndex({index: -1}));

    if (switchToCurrentSelectionTab) {
      globals.dispatch(Actions.showTab({uri: currentSelectionTabUri}));
    }
    globals.dispatch(action);
  }

  setLegacySelection(
    legacySelection: LegacySelection,
    args: Partial<LegacySelectionArgs> = {},
  ): void {
    this._selectionManager.setLegacy(legacySelection);
    this.handleSelectionArgs(args);
  }

  selectSingleEvent(
    trackKey: string,
    eventId: number,
    args: Partial<LegacySelectionArgs> = {},
  ): void {
    this._selectionManager.setEvent(trackKey, eventId);
    this.handleSelectionArgs(args);
  }

  private handleSelectionArgs(args: Partial<LegacySelectionArgs> = {}): void {
    const {
      clearSearch = true,
      switchToCurrentSelectionTab = true,
      pendingScrollId = undefined,
    } = args;
    if (clearSearch) {
      globals.dispatch(Actions.setSearchIndex({index: -1}));
    }
    if (pendingScrollId !== undefined) {
      globals.dispatch(
        Actions.setPendingScrollId({
          pendingScrollId,
        }),
      );
    }
    if (switchToCurrentSelectionTab) {
      globals.dispatch(Actions.showTab({uri: 'current_selection'}));
    }
  }

  clearSelection(): void {
    globals.dispatch(Actions.setSearchIndex({index: -1}));
    this._selectionManager.clear();
  }

  resetForTesting() {
    this._dispatchMultiple = undefined;
    this._timeline = undefined;
    this._serviceWorkerController = undefined;

    // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.
    this._trackDataStore = undefined;
    this._queryResults = undefined;
    this._overviewStore = undefined;
    this._threadMap = undefined;
    this._sliceDetails = undefined;
    this._threadStateDetails = undefined;
    this._aggregateDataStore = undefined;
    this._numQueriesQueued = 0;
    this._metricResult = undefined;
    this._currentSearchResults = {
      eventIds: new Float64Array(0),
      tses: new BigInt64Array(0),
      utids: new Float64Array(0),
      trackKeys: [],
      sources: [],
      totalResults: 0,
    };
  }

  // This variable is set by the is_internal_user.js script if the user is a
  // googler. This is used to avoid exposing features that are not ready yet
  // for public consumption. The gated features themselves are not secret.
  // If a user has been detected as a Googler once, make that sticky in
  // localStorage, so that we keep treating them as such when they connect over
  // public networks.
  get isInternalUser() {
    if (this._isInternalUser === undefined) {
      this._isInternalUser = localStorage.getItem('isInternalUser') === '1';
    }
    return this._isInternalUser;
  }

  set isInternalUser(value: boolean) {
    localStorage.setItem('isInternalUser', value ? '1' : '0');
    this._isInternalUser = value;
    raf.scheduleFullRedraw();
  }

  get testing() {
    return this._testing;
  }

  // Used when switching to the legacy TraceViewer UI.
  // Most resources are cleaned up by replacing the current |window| object,
  // however pending RAFs and workers seem to outlive the |window| and need to
  // be cleaned up explicitly.
  shutdown() {
    raf.shutdown();
  }

  get commandManager(): CommandManager {
    return assertExists(this._cmdManager);
  }

  get tabManager() {
    return this._tabManager;
  }

  get trackManager() {
    return this._trackManager;
  }

  // Offset between t=0 and the configured time domain.
  timestampOffset(): time {
    const fmt = timestampFormat();
    switch (fmt) {
      case TimestampFormat.Timecode:
      case TimestampFormat.Seconds:
        return this.traceContext.start;
      case TimestampFormat.Raw:
      case TimestampFormat.RawLocale:
        return Time.ZERO;
      case TimestampFormat.UTC:
        return this.traceContext.utcOffset;
      case TimestampFormat.TraceTz:
        return this.traceContext.traceTzOffset;
      default:
        const x: never = fmt;
        throw new Error(`Unsupported format ${x}`);
    }
  }

  // Convert absolute time to domain time.
  toDomainTime(ts: time): time {
    return Time.sub(ts, this.timestampOffset());
  }

  async findTimeRangeOfSelection(): Promise<
    Optional<{start: time; end: time}>
  > {
    const sel = globals.state.selection;
    if (sel.kind === 'area') {
      return sel;
    } else if (sel.kind === 'note') {
      const selectedNote = this.state.notes[sel.id];
      // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
      if (selectedNote) {
        const kind = selectedNote.noteType;
        switch (kind) {
          case 'SPAN':
            return {
              start: selectedNote.start,
              end: selectedNote.end,
            };
          case 'DEFAULT':
            return {
              start: selectedNote.timestamp,
              end: Time.add(selectedNote.timestamp, INSTANT_FOCUS_DURATION),
            };
          default:
            assertUnreachable(kind);
        }
      }
    } else if (sel.kind === 'single') {
      const uri = globals.state.tracks[sel.trackKey]?.uri;
      if (uri) {
        const bounds = await globals.trackManager
          .resolveTrackInfo(uri)
          ?.getEventBounds?.(sel.eventId);
        if (bounds) {
          return {
            start: bounds.ts,
            end: Time.add(bounds.ts, bounds.dur),
          };
        }
      }
      return undefined;
    }

    const selection = getLegacySelection(this.state);
    if (selection === null) {
      return undefined;
    }

    if (selection.kind === 'SCHED_SLICE' || selection.kind === 'SLICE') {
      const slice = this.sliceDetails;
      return findTimeRangeOfSlice(slice);
    } else if (selection.kind === 'THREAD_STATE') {
      const threadState = this.threadStateDetails;
      return findTimeRangeOfSlice(threadState);
    } else if (selection.kind === 'LOG') {
      // TODO(hjd): Make focus selection work for logs.
    } else if (selection.kind === 'GENERIC_SLICE') {
      return findTimeRangeOfSlice({
        ts: selection.start,
        dur: selection.duration,
      });
    }

    return undefined;
  }
}

interface SliceLike {
  ts: time;
  dur: duration;
}

// Returns the start and end points of a slice-like object If slice is instant
// or incomplete, dummy time will be returned which instead.
function findTimeRangeOfSlice(slice: Partial<SliceLike>): {
  start: time;
  end: time;
} {
  if (exists(slice.ts) && exists(slice.dur)) {
    if (slice.dur === -1n) {
      return {
        start: slice.ts,
        end: Time.add(slice.ts, INCOMPLETE_SLICE_DURATION),
      };
    } else if (slice.dur === 0n) {
      return {
        start: slice.ts,
        end: Time.add(slice.ts, INSTANT_FOCUS_DURATION),
      };
    } else {
      return {start: slice.ts, end: Time.add(slice.ts, slice.dur)};
    }
  } else {
    return {start: Time.INVALID, end: Time.INVALID};
  }
}

// Returns the time span of the current selection, or the visible window if
// there is no current selection.
export async function getTimeSpanOfSelectionOrVisibleWindow(): Promise<TimeSpan> {
  const range = await globals.findTimeRangeOfSelection();
  if (exists(range)) {
    return new TimeSpan(range.start, range.end);
  } else {
    return globals.timeline.visibleWindow.toTimeSpan();
  }
}

export const globals = new Globals();
