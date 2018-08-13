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
        ready: false,
        source: action.file,
      };
      nextState.route = `/viewer`;

      return nextState;
    }

    case 'OPEN_TRACE_FROM_URL': {
      const nextState = {...state};
      nextState.engines = {...state.engines};
      const id = `${nextState.nextId++}`;
      nextState.engines[id] = {
        id,
        ready: false,
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
        maxDepth: 1,
        cpu: action.cpu,
      };
      nextState.displayedTrackIds.push(id);
      return nextState;
    }

    // TODO: 'ADD_CHROME_TRACK' string should be a shared const.
    case 'ADD_CHROME_TRACK': {
      const nextState = {...state};
      nextState.tracks = {...state.tracks};
      const id = `${nextState.nextId++}`;
      nextState.tracks[id] = {
        id,
        engineId: action.engineId,
        kind: action.trackKind,
        name: `${action.threadName}`,
        // TODO(dproy): This should be part of published information.
        maxDepth: action.maxDepth,
        cpu: 0,  // TODO: Remove this after we have kind specific state.
        upid: action.upid,
        utid: action.utid,
      };
      nextState.displayedTrackIds.push(id);
      return nextState;
    }

    case 'EXECUTE_QUERY': {
      const nextState = {...state};
      nextState.queries = {...state.queries};
      nextState.queries[action.queryId] = {
        id: action.queryId,
        engineId: action.engineId,
        query: action.query,
      };
      return nextState;
    }

    case 'DELETE_QUERY': {
      const nextState = {...state};
      nextState.queries = {...state.queries};
      delete nextState.queries[action.queryId];
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

    case 'SET_ENGINE_READY': {
      const nextState = {...state};  // Creates a shallow copy.
      nextState.engines = {...state.engines};
      nextState.engines[action.engineId].ready = true;
      return nextState;
    }

    case 'CREATE_PERMALINK': {
      const nextState = {...state};
      nextState.permalink = {
        state,
      };
      return nextState;
    }

    case 'SET_STATE': {
      return action.newState;
    }

    case 'SET_TRACE_TIME': {
      const nextState = {...state};
      nextState.traceTime.startSec = action.startSec;
      nextState.traceTime.endSec = action.endSec;
      return nextState;
    }

    default:
      break;
  }
  return state;
}
