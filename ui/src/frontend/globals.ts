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

import {assertExists} from '../base/logging';
import {createStore, Store} from '../base/store';
import {duration, Time, time, TimeSpan} from '../base/time';
import {Actions, DeferredAction} from '../common/actions';
import {AggregateData} from '../common/aggregation_data';
import {Args} from '../common/arg_types';
import {CommandManagerImpl} from '../core/command_manager';
import {
  ConversionJobName,
  ConversionJobStatus,
} from '../common/conversion_jobs';
import {createEmptyState} from '../common/empty_state';
import {EngineConfig, State} from '../common/state';
import {TabManagerImpl} from '../core/tab_manager';
import {TimestampFormat, timestampFormat} from '../core/timestamp_format';
import {TrackManagerImpl} from '../core/track_manager';
import {setPerfHooks} from '../core/perf';
import {raf} from '../core/raf_scheduler';
import {ServiceWorkerController} from './service_worker_controller';
import {Engine, EngineBase} from '../trace_processor/engine';
import {HttpRpcState} from '../trace_processor/http_rpc_engine';
import type {Analytics} from './analytics';
import {TimelineImpl} from '../core/timeline';
import {SliceSqlId} from '../trace_processor/sql_utils/core_types';
import {SelectionManagerImpl} from '../core/selection_manager';
import {exists} from '../base/utils';
import {OmniboxManagerImpl} from '../core/omnibox_manager';
import {SerializedAppState} from '../common/state_serialization_schema';
import {getServingRoot} from '../base/http_utils';
import {
  createSearchOverviewTrack,
  SearchOverviewTrack,
} from './search_overview_track';
import {TraceInfo} from '../public/trace_info';
import {Registry} from '../base/registry';
import {SidebarMenuItem} from '../public/sidebar';
import {Workspace, WorkspaceManager} from '../public/workspace';
import {ratelimit} from './rate_limiters';
import {NoteManagerImpl} from '../core/note_manager';
import {SearchManagerImpl} from '../core/search_manager';
import {SearchResult} from '../public/search';
import {selectCurrentSearchResult} from './search_handler';
import {WorkspaceManagerImpl} from '../core/workspace_manager';
import {ScrollHelper} from '../core/scroll_helper';
import {setScrollToFunction} from '../public/scroll_helper';

type DispatchMultiple = (actions: DeferredAction[]) => void;
type TrackDataStore = Map<string, {}>;
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

export const defaultTraceContext: TraceInfo = {
  traceTitle: '',
  traceUrl: '',
  start: Time.ZERO,
  end: Time.fromSeconds(10),
  realtimeOffset: Time.ZERO,
  utcOffset: Time.ZERO,
  traceTzOffset: Time.ZERO,
  cpus: [],
  gpuCount: 0,
  source: {type: 'URL', url: ''},
};

interface SqlModule {
  readonly name: string;
  readonly sql: string;
}

interface SqlPackage {
  readonly name: string;
  readonly modules: SqlModule[];
}

/**
 * Global accessors for state/dispatch in the frontend.
 */
class Globals {
  readonly root = getServingRoot();

  private _testing = false;
  private _dispatchMultiple?: DispatchMultiple = undefined;
  private _store = createStore<State>(createEmptyState());
  private _timeline: TimelineImpl;
  private _searchManager = new SearchManagerImpl();
  private _serviceWorkerController?: ServiceWorkerController = undefined;
  private _logging?: Analytics = undefined;
  private _isInternalUser: boolean | undefined = undefined;

  // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.
  private _trackDataStore?: TrackDataStore = undefined;
  private _overviewStore?: OverviewStore = undefined;
  private _aggregateDataStore?: AggregateDataStore = undefined;
  private _threadMap?: ThreadMap = undefined;
  private _connectedFlows?: Flow[] = undefined;
  private _selectedFlows?: Flow[] = undefined;
  private _visibleFlowCategories?: Map<string, boolean> = undefined;
  private _numQueriesQueued = 0;
  private _bufferUsage?: number = undefined;
  private _recordingLog?: string = undefined;
  private _traceErrors?: number = undefined;
  private _metricError?: string = undefined;
  private _jobStatus?: Map<ConversionJobName, ConversionJobStatus> = undefined;
  private _embeddedMode?: boolean = undefined;
  private _hideSidebar?: boolean = undefined;
  private _cmdManager = new CommandManagerImpl();
  private _tabManager = new TabManagerImpl();
  private _trackManager = new TrackManagerImpl();
  private _selectionManager = new SelectionManagerImpl();
  private _noteManager = new NoteManagerImpl();
  private _hasFtrace: boolean = false;
  private _searchOverviewTrack?: SearchOverviewTrack;
  private _workspaceManager = new WorkspaceManagerImpl();
  readonly omnibox = new OmniboxManagerImpl();

  httpRpcState: HttpRpcState = {connected: false};
  showPanningHint = false;
  permalinkHash?: string;
  showTraceErrorPopup = true;
  extraSqlPackages: SqlPackage[] = [];

  traceContext = defaultTraceContext;

  readonly sidebarMenuItems = new Registry<SidebarMenuItem>((m) => m.commandId);

  get workspace(): Workspace {
    return this._workspaceManager.currentWorkspace;
  }

  get workspaceManager(): WorkspaceManager {
    return this._workspaceManager;
  }

  // This is the app's equivalent of a plugin's onTraceLoad() function.
  // TODO(stevegolton): Eventually initialization that should be done on trace
  // load should be moved into here, and then we can remove TraceController
  // entirely
  async onTraceLoad(engine: Engine, traceCtx: TraceInfo): Promise<void> {
    this.traceContext = traceCtx;

    // Reset workspaces
    this._workspaceManager = new WorkspaceManagerImpl();

    const {start, end} = traceCtx;
    this._timeline = new TimelineImpl(new TimeSpan(start, end));
    this._timeline.retriggerControllersOnChange = () =>
      ratelimit(() => this.store.edit(() => {}), 50);

    // Reset the trackManager - this clears out the cache and any registered
    // tracks
    this._trackManager = new TrackManagerImpl();

    const scrollHelper = new ScrollHelper(
      traceCtx,
      this._timeline,
      this._workspaceManager.currentWorkspace,
      this._trackManager,
    );
    setScrollToFunction((args) => scrollHelper.scrollTo(args));

    this._searchManager = new SearchManagerImpl({
      timeline: this._timeline,
      trackManager: this._trackManager,
      workspace: this._workspaceManager.currentWorkspace,
      engine,
      onResultStep: (step: SearchResult) => {
        selectCurrentSearchResult(step, this._selectionManager, scrollHelper);
      },
    });

    this._selectionManager = new SelectionManagerImpl({
      engine,
      trackManager: this._trackManager,
      noteManager: this._noteManager,
      scrollHelper,
      onSelectionChange: (_, opts) => {
        const {clearSearch = true, switchToCurrentSelectionTab = true} = opts;
        if (clearSearch) {
          this.searchManager.reset();
        }
        if (switchToCurrentSelectionTab) {
          globals.tabManager.showCurrentSelectionTab();
        }
        // pendingScrollId is handled by SelectionManager internally.

        // TODO(primiano): this is temporarily necessary until we kill
        // controllers. The flow controller needs to be re-kicked when we change
        // the selection.
        globals.dispatch(Actions.runControllers({}));
      },
    });

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
    this._searchOverviewTrack = await createSearchOverviewTrack(
      engine,
      this.searchManager,
      this.timeline,
    );
  }

  // Used for permalink load by trace_controller.ts.
  restoreAppStateAfterTraceLoad?: SerializedAppState;

  // TODO(hjd): Remove once we no longer need to update UUID on redraw.
  private _publishRedraw?: () => void = undefined;

  engines = new Map<string, EngineBase>();

  constructor() {
    const {start, end} = defaultTraceContext;
    this._timeline = new TimelineImpl(new TimeSpan(start, end));
  }

  initialize(
    dispatchMultiple: DispatchMultiple,
    initAnalytics: () => Analytics,
  ) {
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

    // TODO(stevegolton): This is a mess. We should just inject this object in,
    // instead of passing in a function. The only reason this is done like this
    // is because the current implementation of initAnalytics depends on the
    // state of globals.testing, so this needs to be set before we run the
    // function.
    this._logging = initAnalytics();

    // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.
    this._trackDataStore = new Map<string, {}>();
    this._overviewStore = new Map<string, QuantizedLoad[]>();
    this._aggregateDataStore = new Map<string, AggregateData>();
    this._threadMap = new Map<number, ThreadDesc>();
    this._connectedFlows = [];
    this._selectedFlows = [];
    this._visibleFlowCategories = new Map<string, boolean>();
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

  get searchManager() {
    return this._searchManager;
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

  get threads() {
    return assertExists(this._threadMap);
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

  get commandManager(): CommandManagerImpl {
    return assertExists(this._cmdManager);
  }

  get tabManager() {
    return this._tabManager;
  }

  get trackManager() {
    return this._trackManager;
  }

  get selectionManager() {
    return this._selectionManager;
  }

  get noteManager() {
    return this._noteManager;
  }

  // Offset between t=0 and the configured time domain.
  timestampOffset(): time {
    const fmt = timestampFormat();
    switch (fmt) {
      case TimestampFormat.Timecode:
      case TimestampFormat.Seconds:
      case TimestampFormat.Milliseoncds:
      case TimestampFormat.Microseconds:
        return this.traceContext.start;
      case TimestampFormat.TraceNs:
      case TimestampFormat.TraceNsLocale:
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
}

// Returns the time span of the current selection, or the visible window if
// there is no current selection.
export async function getTimeSpanOfSelectionOrVisibleWindow(): Promise<TimeSpan> {
  const range = await globals.selectionManager.findTimeRangeOfSelection();
  if (exists(range)) {
    return new TimeSpan(range.start, range.end);
  } else {
    return globals.timeline.visibleWindow.toTimeSpan();
  }
}

export const globals = new Globals();
