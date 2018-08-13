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
import {Action} from '../common/actions';
import {State} from '../common/state';

import {FrontendLocalState} from './frontend_local_state';
import {RafScheduler} from './raf_scheduler';

type Dispatch = (action: Action) => void;
type TrackDataStore = Map<string, {}>;
type QueryResultsStore = Map<string, {}>;

/**
 * Global accessors for state/dispatch in the frontend.
 */
class Globals {
  private _dispatch?: Dispatch = undefined;
  private _state?: State = undefined;
  private _trackDataStore?: TrackDataStore = undefined;
  private _queryResults?: QueryResultsStore = undefined;
  private _frontendLocalState?: FrontendLocalState = undefined;
  private _rafScheduler?: RafScheduler = undefined;

  initialize(
      dispatch?: Dispatch, state?: State, trackDataStore?: TrackDataStore,
      queryResults?: QueryResultsStore, frontendLocalState?: FrontendLocalState,
      rafScheduler?: RafScheduler) {
    this._dispatch = dispatch;
    this._state = state;
    this._trackDataStore = trackDataStore;
    this._queryResults = queryResults;
    this._frontendLocalState = frontendLocalState;
    this._rafScheduler = rafScheduler;
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

  get trackDataStore(): TrackDataStore {
    return assertExists(this._trackDataStore);
  }

  get queryResults(): QueryResultsStore {
    return assertExists(this._queryResults);
  }

  get frontendLocalState() {
    return assertExists(this._frontendLocalState);
  }

  get rafScheduler() {
    return assertExists(this._rafScheduler);
  }

  resetForTesting() {
    this.initialize(
        undefined, undefined, undefined, undefined, undefined, undefined);
  }
}

export const globals = new Globals();
