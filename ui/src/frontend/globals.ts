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
import {createEmptyState} from '../common/empty_state';
import {State} from '../common/state';
import {setPerfHooks} from '../core/perf';
import {raf} from '../core/raf_scheduler';
import {ServiceWorkerController} from './service_worker_controller';
import {HttpRpcState} from '../trace_processor/http_rpc_engine';
import {getServingRoot} from '../base/http_utils';
import {AppImpl} from '../core/app_impl';

type DispatchMultiple = (actions: DeferredAction[]) => void;
type TrackDataStore = Map<string, {}>;

/**
 * Global accessors for state/dispatch in the frontend.
 */
class Globals {
  private _dispatchMultiple?: DispatchMultiple = undefined;
  private _store = createStore<State>(createEmptyState());
  private _serviceWorkerController?: ServiceWorkerController = undefined;
  private _isInternalUser: boolean | undefined = undefined;

  // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.
  private _trackDataStore?: TrackDataStore = undefined;
  private _bufferUsage?: number = undefined;
  private _recordingLog?: string = undefined;
  httpRpcState: HttpRpcState = {connected: false};
  showPanningHint = false;

  // TODO(hjd): Remove once we no longer need to update UUID on redraw.
  private _publishRedraw?: () => void = undefined;

  initialize(dispatchMultiple: DispatchMultiple) {
    this._dispatchMultiple = dispatchMultiple;

    setPerfHooks(
      () => this.state.perfDebug,
      () => this.dispatch(Actions.togglePerfDebug({})),
    );

    this._serviceWorkerController = new ServiceWorkerController(
      getServingRoot(),
    );

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

  get serviceWorkerController() {
    return assertExists(this._serviceWorkerController);
  }

  // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.

  get trackDataStore(): TrackDataStore {
    return assertExists(this._trackDataStore);
  }

  get bufferUsage() {
    return this._bufferUsage;
  }

  get recordingLog() {
    return this._recordingLog;
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

  // Used when switching to the legacy TraceViewer UI.
  // Most resources are cleaned up by replacing the current |window| object,
  // however pending RAFs and workers seem to outlive the |window| and need to
  // be cleaned up explicitly.
  shutdown() {
    raf.shutdown();
  }
}

export const globals = new Globals();
