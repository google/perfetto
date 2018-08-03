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

    case 'OPEN_TRACE_FROM_FILE': {
      const nextState = {...state};
      nextState.engines = {...state.engines};
      const id = `${nextState.nextId++}`;
      nextState.engines[id] = {
        id,
        source: action.file,
      };
      nextState.route = `/viewer`;

      return nextState;
    }

    case 'OPEN_TRACE': {
      const nextState = {...state};
      nextState.engines = {...state.engines};
      const id = `${nextState.nextId++}`;
      nextState.engines[id] = {
        id,
        source: action.url,
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
        name: `Cpu Track ${id}`,
        // TODO(hjd): Should height be part of published information?
        height: 73,
        cpu: action.cpu,
      };
      nextState.displayedTrackIds.push(id);
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

    case 'MOVE_TRACK':
      if (!state.displayedTrackIds.includes(action.trackId) ||
          !action.direction) {
        throw new Error(
            'Trying to move a track that does not exist' +
            ' or not providing a direction to move to.');
      }
      const nextState = {...state};  // Creates a shallow copy.
      // Copy the displayedTrackIds to prevent side effects.
      nextState.displayedTrackIds = state.displayedTrackIds.slice();

      const oldIndex = state.displayedTrackIds.indexOf(action.trackId);
      const newIndex = action.direction === 'up' ? oldIndex - 1 : oldIndex + 1;
      const swappedTrackId = state.displayedTrackIds[newIndex];

      if (!swappedTrackId) {
        break;
      }
      nextState.displayedTrackIds[newIndex] = action.trackId;
      nextState.displayedTrackIds[oldIndex] = swappedTrackId;

      return nextState;

    default:
      break;
  }
  return state;
}
