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

import {applyPatches, Patch} from 'immer';

import {assertExists} from '../base/logging';
import {Remote} from '../base/remote';
import {DeferredAction} from '../common/actions';
import {createEmptyState, State} from '../common/state';
import {ControllerAny} from './controller';

type PublishKinds = 'OverviewData'|'TrackData'|'Threads'|'QueryResult'|
    'LegacyTrace'|'SliceDetails'|'CounterDetails'|'HeapProfileDetails'|
    'HeapProfileFlamegraph'|'FileDownload'|'Loading'|'Search'|'BufferUsage'|
    'RecordingLog'|'SearchResult'|'AggregateData'|'CpuProfileDetails'|
    'TraceErrors'|'UpdateChromeCategories'|'ConnectedFlows'|'SelectedFlows'|
    'ThreadStateDetails'|'MetricError'|'MetricResult'|'HasFtrace'|
    'ConversionJobStatusUpdate';

export interface App {
  state: State;
  dispatch(action: DeferredAction): void;
  publish(what: PublishKinds, data: {}, transferList?: Array<{}>): void;
}

/**
 * Global accessors for state/dispatch in the controller.
 */
class Globals implements App {
  private _state?: State;
  private _rootController?: ControllerAny;
  private _frontend?: Remote;
  private _runningControllers = false;

  initialize(rootController: ControllerAny, frontendProxy: Remote) {
    this._rootController = rootController;
    this._frontend = frontendProxy;
    this._state = createEmptyState();
  }

  dispatch(action: DeferredAction): void {
    this.dispatchMultiple([action]);
  }

  // Send the passed dispatch actions to the frontend. The frontend logic
  // will run the actions, compute the new state and invoke patchState() so
  // our copy is updated.
  dispatchMultiple(actions: DeferredAction[]): void {
    assertExists(this._frontend).send<void>('dispatchMultiple', [actions]);
  }

  // This is called by the frontend logic which now owns and handle the
  // source-of-truth state, to give us an update on the newer state updates.
  patchState(patches: Patch[]): void {
    this._state = applyPatches(this._state, patches);
    this.runControllers();
  }

  private runControllers() {
    if (this._runningControllers) throw new Error('Re-entrant call detected');

    // Run controllers locally until all state machines reach quiescence.
    let runAgain = true;
    for (let iter = 0; runAgain; iter++) {
      if (iter > 100) throw new Error('Controllers are stuck in a livelock');
      this._runningControllers = true;
      try {
        runAgain = assertExists(this._rootController).invoke();
      } finally {
        this._runningControllers = false;
      }
    }
  }

  // TODO: this needs to be cleaned up.
  publish(what: PublishKinds, data: {}, transferList?: Transferable[]) {
    assertExists(this._frontend)
        .send<void>(`publish${what}`, [data], transferList);
  }

  // Returns the port of the MessageChannel that can be used to communicate with
  // the Wasm Engine (issue SQL queries and retrieve results).
  resetEngineWorker() {
    const chan = new MessageChannel();
    const port = chan.port1;
    // Invokes resetEngineWorker() in frontend/index.ts. It will spawn a new
    // worker and assign it the passed |port|.
    assertExists(this._frontend).send<void>('resetEngineWorker', [port], [
      port
    ]);
    return chan.port2;
  }

  get state(): State {
    return assertExists(this._state);
  }

  resetForTesting() {
    this._state = undefined;
    this._rootController = undefined;
  }
}

export const globals = new Globals();
