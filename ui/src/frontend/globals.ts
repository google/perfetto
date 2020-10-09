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
import {Actions, DeferredAction} from '../common/actions';
import {AggregateData} from '../common/aggregation_data';
import {MetricResult} from '../common/metric_data';
import {CurrentSearchResults, SearchSummary} from '../common/search_data';
import {CallsiteInfo, createEmptyState, State} from '../common/state';
import {fromNs, toNs} from '../common/time';
import {Analytics, initAnalytics} from '../frontend/analytics';

import {FrontendLocalState} from './frontend_local_state';
import {RafScheduler} from './raf_scheduler';
import {findUiTrackId} from './scroll_helper';
import {ServiceWorkerController} from './service_worker_controller';

type Dispatch = (action: DeferredAction) => void;
type TrackDataStore = Map<string, {}>;
type QueryResultsStore = Map<string, {}>;
type AggregateDataStore = Map<string, AggregateData>;
type Description = Map<string, string>;
type Direction = 'Forward'|'Backward';
export type Arg = string|{kind: 'SLICE', trackId: string, sliceId: number};
export type Args = Map<string, Arg>;
export interface SliceDetails {
  ts?: number;
  dur?: number;
  priority?: number;
  endState?: string;
  cpu?: number;
  id?: number;
  threadStateId?: number;
  utid?: number;
  wakeupTs?: number;
  wakerUtid?: number;
  wakerCpu?: number;
  category?: string;
  name?: string;
  args?: Args;
  description?: Description;
}

export interface FlowPoint {
  trackId: number;

  sliceName: string;
  sliceId: number;
  sliceStartTs: number;
  sliceEndTs: number;

  depth: number;
}

export interface Flow {
  begin: FlowPoint;
  end: FlowPoint;

  category?: string;
  name?: string;
}

export interface CounterDetails {
  startTime?: number;
  value?: number;
  delta?: number;
  duration?: number;
}

export interface ThreadStateDetails {
  ts?: number;
  dur?: number;
  state?: string;
  utid?: number;
  cpu?: number;
  sliceId?: number;
}

export interface HeapProfileDetails {
  type?: string;
  id?: number;
  ts?: number;
  tsNs?: number;
  pid?: number;
  upid?: number;
  flamegraph?: CallsiteInfo[];
  expandedCallsite?: CallsiteInfo;
  viewingOption?: string;
  expandedId?: number;
}

export interface CpuProfileDetails {
  id?: number;
  ts?: number;
  utid?: number;
  stack?: CallsiteInfo[];
}

export interface QuantizedLoad {
  startSec: number;
  endSec: number;
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

/**
 * Global accessors for state/dispatch in the frontend.
 */
class Globals {
  private _dispatch?: Dispatch = undefined;
  private _controllerWorker?: Worker = undefined;
  private _state?: State = undefined;
  private _frontendLocalState?: FrontendLocalState = undefined;
  private _rafScheduler?: RafScheduler = undefined;
  private _serviceWorkerController?: ServiceWorkerController = undefined;
  private _logging?: Analytics = undefined;

  // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.
  private _trackDataStore?: TrackDataStore = undefined;
  private _queryResults?: QueryResultsStore = undefined;
  private _overviewStore?: OverviewStore = undefined;
  private _aggregateDataStore?: AggregateDataStore = undefined;
  private _threadMap?: ThreadMap = undefined;
  private _sliceDetails?: SliceDetails = undefined;
  private _threadStateDetails?: ThreadStateDetails = undefined;
  private _boundFlows?: Flow[] = undefined;
  private _counterDetails?: CounterDetails = undefined;
  private _heapProfileDetails?: HeapProfileDetails = undefined;
  private _cpuProfileDetails?: CpuProfileDetails = undefined;
  private _numQueriesQueued = 0;
  private _bufferUsage?: number = undefined;
  private _recordingLog?: string = undefined;
  private _traceErrors?: number = undefined;
  private _metricError?: string = undefined;
  private _metricResult?: MetricResult = undefined;

  private _currentSearchResults: CurrentSearchResults = {
    sliceIds: [],
    tsStarts: [],
    utids: [],
    trackIds: [],
    sources: [],
    totalResults: 0,
  };
  searchSummary: SearchSummary = {
    tsStarts: new Float64Array(0),
    tsEnds: new Float64Array(0),
    count: new Uint8Array(0),
  };

  // This variable is set by the is_internal_user.js script if the user is a
  // googler. This is used to avoid exposing features that are not ready yet
  // for public consumption. The gated features themselves are not secret.
  isInternalUser = false;

  initialize(dispatch: Dispatch, controllerWorker: Worker) {
    this._dispatch = dispatch;
    this._controllerWorker = controllerWorker;
    this._state = createEmptyState();
    this._frontendLocalState = new FrontendLocalState();
    this._rafScheduler = new RafScheduler();
    this._serviceWorkerController = new ServiceWorkerController();
    this._logging = initAnalytics();

    // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.
    this._trackDataStore = new Map<string, {}>();
    this._queryResults = new Map<string, {}>();
    this._overviewStore = new Map<string, QuantizedLoad[]>();
    this._aggregateDataStore = new Map<string, AggregateData>();
    this._threadMap = new Map<number, ThreadDesc>();
    this._sliceDetails = {};
    this._boundFlows = [];
    this._counterDetails = {};
    this._threadStateDetails = {};
    this._heapProfileDetails = {};
    this._cpuProfileDetails = {};
  }

  get state(): State {
    return assertExists(this._state);
  }

  set state(state: State) {
    this._state = assertExists(state);
  }

  get dispatch(): Dispatch {
    return assertExists(this._dispatch);
  }

  get frontendLocalState() {
    return assertExists(this._frontendLocalState);
  }

  get rafScheduler() {
    return assertExists(this._rafScheduler);
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

  get boundFlows() {
    return assertExists(this._boundFlows);
  }

  set boundFlows(boundFlows: Flow[]) {
    this._boundFlows = assertExists(boundFlows);
  }

  get counterDetails() {
    return assertExists(this._counterDetails);
  }

  set counterDetails(click: CounterDetails) {
    this._counterDetails = assertExists(click);
  }

  get aggregateDataStore(): AggregateDataStore {
    return assertExists(this._aggregateDataStore);
  }

  get heapProfileDetails() {
    return assertExists(this._heapProfileDetails);
  }

  set heapProfileDetails(click: HeapProfileDetails) {
    this._heapProfileDetails = assertExists(click);
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

  getCurResolution() {
    // Truncate the resolution to the closest power of 2 (in nanosecond space).
    // We choose to work in ns space because resolution is consumed be track
    // controllers for quantization and they rely on resolution to be a power
    // of 2 in nanosecond form. This is property does not hold if we work in
    // second space.
    //
    // This effectively means the resolution changes approximately every 6 zoom
    // levels. Logic: each zoom level represents a delta of 0.1 * (visible
    // window span). Therefore, zooming out by six levels is 1.1^6 ~= 2.
    // Similarily, zooming in six levels is 0.9^6 ~= 0.5.
    const pxToSec = this.frontendLocalState.timeScale.deltaPxToDuration(1);
    const pxToNs = Math.max(toNs(pxToSec), 1);
    return fromNs(Math.pow(2, Math.floor(Math.log2(pxToNs))));
  }

  makeSelection(action: DeferredAction<{}>, tabToOpen = 'current_selection') {
    // A new selection should cancel the current search selection.
    globals.frontendLocalState.searchIndex = -1;
    globals.frontendLocalState.currentTab =
        action.type === 'deselect' ? undefined : tabToOpen;
    globals.dispatch(action);
  }

  moveByFlow(direction: Direction) {
    if (!globals.state.currentSelection ||
        globals.state.currentSelection.kind !== 'CHROME_SLICE') {
      return;
    }
    const sliceId = globals.state.currentSelection.id;
    if (sliceId === -1) {
      return;
    }
    let nextSliceId = -1;
    let nextTrackId = -1;

    // Choose any flow
    for (const flow of this.boundFlows) {
      if (flow.begin.sliceId === sliceId && direction === 'Forward') {
        nextSliceId = flow.end.sliceId;
        nextTrackId = flow.end.trackId;
        break;
      }
      if (flow.end.sliceId === sliceId && direction === 'Backward') {
        nextSliceId = flow.begin.sliceId;
        nextTrackId = flow.begin.trackId;
        break;
      }
    }

    const uiTrackId = findUiTrackId(nextTrackId);
    if (uiTrackId) {
      globals.makeSelection(Actions.selectChromeSlice(
          {id: nextSliceId, trackId: uiTrackId, table: 'slice'}));
    }
  }

  resetForTesting() {
    this._dispatch = undefined;
    this._state = undefined;
    this._frontendLocalState = undefined;
    this._rafScheduler = undefined;
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
      sliceIds: [],
      tsStarts: [],
      utids: [],
      trackIds: [],
      sources: [],
      totalResults: 0,
    };
  }

  // Used when switching to the legacy TraceViewer UI.
  // Most resources are cleaned up by replacing the current |window| object,
  // however pending RAFs and workers seem to outlive the |window| and need to
  // be cleaned up explicitly.
  shutdown() {
    this._controllerWorker!.terminate();
    this._rafScheduler!.shutdown();
  }
}

export const globals = new Globals();
