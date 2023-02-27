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

import {ControllerAny} from './controller';

class Globals {
  private _rootController?: ControllerAny;
  private _runningControllers = false;

  initialize(rootController: ControllerAny) {
    this._rootController = rootController;
  }

  runControllers() {
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

  resetForTesting() {
    this._rootController = undefined;
  }
}

export const globals = new Globals();
