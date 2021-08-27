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
import {DeferredAction} from '../common/actions';
import {createEmptyState, State} from '../common/state';
import {globals as frontendGlobals} from '../frontend/globals';
import {ControllerAny} from './controller';

export interface App {
  state: State;
  dispatch(action: DeferredAction): void;
}

/**
 * Global accessors for state/dispatch in the controller.
 */
class Globals implements App {
  private _state?: State;
  private _rootController?: ControllerAny;
  private _runningControllers = false;

  initialize(rootController: ControllerAny) {
    this._rootController = rootController;
    this._state = createEmptyState();
  }

  dispatch(action: DeferredAction): void {
    frontendGlobals.dispatch(action);
  }

  // Send the passed dispatch actions to the frontend. The frontend logic
  // will run the actions, compute the new state and invoke patchState() so
  // our copy is updated.
  dispatchMultiple(actions: DeferredAction[]): void {
    for (const action of actions) {
      this.dispatch(action);
    }
  }

  // This is called by the frontend logic which now owns and handle the
  // source-of-truth state, to give us an update on the newer state updates.
  patchState(patches: Patch[]): void {
    this._state = applyPatches(assertExists(this._state), patches);
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

  get state(): State {
    return assertExists(this._state);
  }

  resetForTesting() {
    this._state = undefined;
    this._rootController = undefined;
  }
}

export const globals = new Globals();
