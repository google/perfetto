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

import {State} from '../common/state';

// TODO(hjd): Type check this better.
// tslint:disable-next-line no-any
export function rootReducer(state: State, action: any): State {
  switch (action.type) {
    case 'NAVIGATE': {
      const nextState = {...state};
      nextState.route = action.route;
      return nextState;
    }

    case 'OPEN_TRACE': {
      const nextState = {...state};
      nextState.engines = {...state.engines};
      const id = `${nextState.nextId++}`;
      nextState.engines[id] = {
        id,
        url: action.url,
      };
      nextState.route = `/viewer`;

      return nextState;
    }

    case 'ADD_TRACK': {
      const nextState = {...state};
      nextState.tracks = {...state.tracks};
      const id = `${nextState.nextId++}`;
      nextState.tracks[id] = {
        id,
        engineId: action.engineId,
        kind: action.trackKind,
        name: 'Cpu Track',
        // TODO(hjd): Should height be part of published information?
        height: 73,
        cpu: action.cpu,
      };
      return nextState;
    }

    case 'EXECUTE_QUERY': {
      const nextState = {...state};
      nextState.queries = {...state.queries};
      const id = `${nextState.nextId++}`;
      nextState.queries[id] = {
        id,
        engineId: action.engineId,
        query: action.query,
      };
      return nextState;
    }

    default:
      break;
  }
  return state;
}
