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
import {time} from '../base/time';
import {Actions, DeferredAction} from '../common/actions';
import {CommandManagerImpl} from '../core/command_manager';
import {
  ConversionJobName,
  ConversionJobStatus,
} from '../common/conversion_jobs';
import {createEmptyState} from '../common/empty_state';
import {EngineConfig, State} from '../common/state';
import {setPerfHooks} from '../core/perf';
import {raf} from '../core/raf_scheduler';
import {ServiceWorkerController} from './service_worker_controller';
import {EngineBase} from '../trace_processor/engine';
import {HttpRpcState} from '../trace_processor/http_rpc_engine';
import type {Analytics} from './analytics';
import {SerializedAppState} from '../common/state_serialization_schema';
import {getServingRoot} from '../base/http_utils';
import {Workspace} from '../public/workspace';
import {ratelimit} from './rate_limiters';
import {setRerunControllersFunction, TraceImpl} from '../core/trace_impl';
import {AppImpl} from '../core/app_impl';
import {createFakeTraceImpl} from '../common/fake_trace_impl';

type DispatchMultiple = (actions: DeferredAction[]) => void;
type TrackDataStore = Map<string, {}>;

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

  private _trace: TraceImpl;
  private _testing = false;
  private _dispatchMultiple?: DispatchMultiple = undefined;
  private _store = createStore<State>(createEmptyState());
  private _serviceWorkerController?: ServiceWorkerController = undefined;
  private _logging?: Analytics = undefined;
  private _isInternalUser: boolean | undefined = undefined;

  // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.
  private _trackDataStore?: TrackDataStore = undefined;
  private _overviewStore?: OverviewStore = undefined;
  private _threadMap?: ThreadMap = undefined;
  private _bufferUsage?: number = undefined;
  private _recordingLog?: string = undefined;
  private _metricError?: string = undefined;
  private _jobStatus?: Map<ConversionJobName, ConversionJobStatus> = undefined;
  private _embeddedMode?: boolean = undefined;
  private _hideSidebar?: boolean = undefined;
  private _hasFtrace: boolean = false;
  private _currentTraceId = '';
  httpRpcState: HttpRpcState = {connected: false};
  showPanningHint = false;
  permalinkHash?: string;
  extraSqlPackages: SqlPackage[] = [];

  get workspace(): Workspace {
    return this._trace?.workspace;
  }

  // This is the app's equivalent of a plugin's onTraceLoad() function.
  // TODO(primiano): right now this is used to inject the TracImpl class into
  // globals, so it can hop consistently all its accessors to it. Once globals
  // is gone, figure out what to do with createSearchOverviewTrack().
  async onTraceLoad(trace: TraceImpl): Promise<void> {
    this._trace = trace;

    this._trace.timeline.retriggerControllersOnChange = () =>
      ratelimit(() => this.store.edit(() => {}), 50);

    this._currentTraceId = trace.engine.engineId;
  }

  // Used for permalink load by trace_controller.ts.
  restoreAppStateAfterTraceLoad?: SerializedAppState;

  // TODO(hjd): Remove once we no longer need to update UUID on redraw.
  private _publishRedraw?: () => void = undefined;

  engines = new Map<string, EngineBase>();

  constructor() {
    // TODO(primiano): we do this to avoid making all our members possibly
    // undefined, which would cause a drama of if (!=undefined) all over the
    // code. This is not pretty, but this entire file is going to be nuked from
    // orbit soon.
    this._trace = createFakeTraceImpl();

    // We just want an empty instance of TraceImpl but don't want to mark it
    // as the current trace, otherwise this will trigger the plugins' OnLoad().
    AppImpl.instance.closeCurrentTrace();

    setRerunControllersFunction(() =>
      this.dispatch(Actions.runControllers({})),
    );
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
    // TODO(primiano): for posterity: these assignments below are completely
    // pointless and could be done as member variable initializers, as
    // initialize() is only called ever once. (But then i'm going to kill this
    // entire file soon).
    this._trackDataStore = new Map<string, {}>();
    this._overviewStore = new Map<string, QuantizedLoad[]>();
    this._threadMap = new Map<number, ThreadDesc>();
    this.engines.clear();
  }

  get publishRedraw(): () => void {
    return this._publishRedraw || (() => {});
  }

  set publishRedraw(f: () => void) {
    this._publishRedraw = f;
  }

  get state(): State {
    return this._store.state;
  }

  get store(): Store<State> {
    return this._store;
  }

  dispatch(action: DeferredAction) {
    this.dispatchMultiple([action]);
  }

  dispatchMultiple(actions: DeferredAction[]) {
    assertExists(this._dispatchMultiple)(actions);
  }

  get trace() {
    return this._trace;
  }

  get timeline() {
    return this._trace.timeline;
  }

  get searchManager() {
    return this._trace.search;
  }

  get logging() {
    return assertExists(this._logging);
  }

  get serviceWorkerController() {
    return assertExists(this._serviceWorkerController);
  }

  // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.

  // TODO(primiano): this should be really renamed to traceInfo, but doing so
  // creates extra churn. Not worth it as we are going to get rid of this file
  // soon.
  get traceContext() {
    return this._trace.traceInfo;
  }

  get overviewStore(): OverviewStore {
    return assertExists(this._overviewStore);
  }

  get trackDataStore(): TrackDataStore {
    return assertExists(this._trackDataStore);
  }

  get threads() {
    return assertExists(this._threadMap);
  }

  get metricError() {
    return this._metricError;
  }

  setMetricError(arg: string) {
    this._metricError = arg;
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

  get currentTraceId() {
    return this._currentTraceId;
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
    return AppImpl.instance.commands;
  }

  get tabManager() {
    return this._trace.tabs;
  }

  get trackManager() {
    return this._trace.tracks;
  }

  get selectionManager() {
    return this._trace.selection;
  }

  get noteManager() {
    return this._trace.notes;
  }
}

export const globals = new Globals();
