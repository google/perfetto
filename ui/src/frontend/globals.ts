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

import {Action} from '../common/actions';
import {State} from '../common/state';
import {Engine} from '../controller/engine';

type Dispatch = (action: Action) => void;

/**
 * Global accessors for state/dispatch in the frontend.
 */
class Globals {
  _dispatch?: Dispatch = undefined;
  _state?: State = undefined;

  get state(): State {
    if (this._state === undefined) throw new Error('Global not set');
    return this._state;
  }

  set state(value: State) {
    this._state = value;
  }

  get dispatch(): Dispatch {
    if (this._dispatch === undefined) throw new Error('Global not set');
    return this._dispatch;
  }

  set dispatch(value: Dispatch) {
    this._dispatch = value;
  }

  resetForTesting() {
    this._state = undefined;
    this._dispatch = undefined;
  }
}

// TODO(hjd): Temporary while bringing up controller worker.
export const gEngines = new Map<string, Engine>();

export const globals = new Globals();
