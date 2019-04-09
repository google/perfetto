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
import {createEmptyState, State} from '../common/state';

import {FrontendLocalState} from './frontend_local_state';
import {RafScheduler} from './raf_scheduler';

type Dispatch = (action: DeferredAction) => void;
type TrackDataStore = Map<string, {}>;
type QueryResultsStore = Map<string, {}>;
export interface SliceDetails {
  ts?: number;
  dur?: number;
  priority?: number;
  endState?: string;
  wakeupTs?: number;
  wakerUtid?: number;
  wakerCpu?: number;
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

  // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.
  private _trackDataStore?: TrackDataStore = undefined;
  private _queryResults?: QueryResultsStore = undefined;
  private _overviewStore?: OverviewStore = undefined;
  private _threadMap?: ThreadMap = undefined;
  private _sliceDetails?: SliceDetails = undefined;
  private _pendingTrackRequests?: Set<string> = undefined;

  initialize(dispatch: Dispatch, controllerWorker: Worker) {
    this._dispatch = dispatch;
    this._controllerWorker = controllerWorker;
    this._state = createEmptyState();
    this._frontendLocalState = new FrontendLocalState();
    this._rafScheduler = new RafScheduler();

    // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.
    this._trackDataStore = new Map<string, {}>();
    this._queryResults = new Map<string, {}>();
    this._overviewStore = new Map<string, QuantizedLoad[]>();
    this._threadMap = new Map<number, ThreadDesc>();
    this._sliceDetails = {};
    this._pendingTrackRequests = new Set<string>();
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

  setTrackData(id: string, data: {}) {
    this.trackDataStore.set(id, data);
    assertExists(this._pendingTrackRequests).delete(id);
  }

  getCurResolution() {
    // Truncate the resolution to the closest power of 10.
    const resolution = this.frontendLocalState.timeScale.deltaPxToDuration(1);
    return Math.pow(10, Math.floor(Math.log10(resolution)));
  }

  requestTrackData(trackId: string) {
    const pending = assertExists(this._pendingTrackRequests);
    if (pending.has(trackId)) return;

    const {visibleWindowTime} = globals.frontendLocalState;
    const resolution = this.getCurResolution();
    const start = visibleWindowTime.start - visibleWindowTime.duration;
    const end = visibleWindowTime.end + visibleWindowTime.duration;

    pending.add(trackId);
    globals.dispatch(Actions.reqTrackData({
      trackId,
      start,
      end,
      resolution,
    }));
  }

  resetForTesting() {
    this._dispatch = undefined;
    this._state = undefined;
    this._frontendLocalState = undefined;
    this._rafScheduler = undefined;

    // TODO(hjd): Unify trackDataStore, queryResults, overviewStore, threads.
    this._trackDataStore = undefined;
    this._queryResults = undefined;
    this._overviewStore = undefined;
    this._threadMap = undefined;
    this._sliceDetails = undefined;
    this._pendingTrackRequests = undefined;
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
