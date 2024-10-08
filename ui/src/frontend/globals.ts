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
import {Actions, DeferredAction} from '../common/actions';
import {CommandManagerImpl} from '../core/command_manager';
import {
  ConversionJobName,
  ConversionJobStatus,
} from '../common/conversion_jobs';
import {createEmptyState} from '../common/empty_state';
import {State} from '../common/state';
import {setPerfHooks} from '../core/perf';
import {raf} from '../core/raf_scheduler';
import {ServiceWorkerController} from './service_worker_controller';
import {HttpRpcState} from '../trace_processor/http_rpc_engine';
import type {Analytics} from './analytics';
import {getServingRoot} from '../base/http_utils';
import {Workspace} from '../public/workspace';
import {TraceImpl} from '../core/trace_impl';
import {AppImpl} from '../core/app_impl';
import {createFakeTraceImpl} from '../core/fake_trace_impl';

type DispatchMultiple = (actions: DeferredAction[]) => void;
type TrackDataStore = Map<string, {}>;

/**
 * Global accessors for state/dispatch in the frontend.
 */
class Globals {
  private _initialFakeTrace?: TraceImpl;
  private _testing = false;
  private _dispatchMultiple?: DispatchMultiple = undefined;
  private _store = createStore<State>(createEmptyState());
  private _serviceWorkerController?: ServiceWorkerController = undefined;
  private _logging?: Analytics = undefined;
  private _isInternalUser: boolean | undefined = undefined;

  // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.
  private _trackDataStore?: TrackDataStore = undefined;
  private _bufferUsage?: number = undefined;
  private _recordingLog?: string = undefined;
  private _jobStatus?: Map<ConversionJobName, ConversionJobStatus> = undefined;
  private _embeddedMode?: boolean = undefined;
  private _hideSidebar?: boolean = undefined;
  httpRpcState: HttpRpcState = {connected: false};
  showPanningHint = false;
  permalinkHash?: string;

  // TODO(hjd): Remove once we no longer need to update UUID on redraw.
  private _publishRedraw?: () => void = undefined;

  initialize(
    dispatchMultiple: DispatchMultiple,
    initAnalytics: () => Analytics,
  ) {
    this._dispatchMultiple = dispatchMultiple;

    // TODO(primiano): we do this to avoid making all our members possibly
    // undefined, which would cause a drama of if (!=undefined) all over the
    // code. This is not pretty, but this entire file is going to be nuked from
    // orbit soon.
    this._initialFakeTrace = createFakeTraceImpl();

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
  }

  get root() {
    return AppImpl.instance.rootUrl;
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
    const trace = AppImpl.instance.trace;
    return trace ?? assertExists(this._initialFakeTrace);
  }

  get timeline() {
    return this.trace.timeline;
  }

  get searchManager() {
    return this.trace.search;
  }

  get logging() {
    return assertExists(this._logging);
  }

  get serviceWorkerController() {
    return assertExists(this._serviceWorkerController);
  }

  get workspace(): Workspace {
    return this.trace.workspace;
  }

  // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.

  // TODO(primiano): this should be really renamed to traceInfo, but doing so
  // creates extra churn. Not worth it as we are going to get rid of this file
  // soon.
  get traceContext() {
    return this.trace.traceInfo;
  }

  get trackDataStore(): TrackDataStore {
    return assertExists(this._trackDataStore);
  }

  get bufferUsage() {
    return this._bufferUsage;
  }

  get recordingLog() {
    return this._recordingLog;
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

  get extraSqlPackages() {
    return AppImpl.instance.extraSqlPackages;
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
    return this.trace.tabs;
  }

  get trackManager() {
    return this.trace.tracks;
  }

  get selectionManager() {
    return this.trace.selection;
  }

  get noteManager() {
    return this.trace.notes;
  }
}

export const globals = new Globals();
