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

import {State} from './state';
import {TimeSpan} from './time';

export interface Action { type: string; }

export function openTraceFromUrl(url: string) {
  return {
    type: 'OPEN_TRACE_FROM_URL',
    url,
  };
}

export function openTraceFromFile(file: File) {
  return {
    type: 'OPEN_TRACE_FROM_FILE',
    file,
  };
}

// TODO(hjd): Remove CPU and add a generic way to handle track specific state.
export function addTrack(
    engineId: string, trackKind: string, name: string, config: {}) {
  return {
    type: 'ADD_TRACK',
    engineId,
    trackKind,
    name,
    config,
  };
}

export function requestTrackData(
    trackId: string, start: number, end: number, resolution: number) {
  return {type: 'REQ_TRACK_DATA', trackId, start, end, resolution};
}

export function clearTrackDataRequest(trackId: string) {
  return {type: 'CLEAR_TRACK_DATA_REQ', trackId};
}

export function executeQuery(engineId: string, queryId: string, query: string) {
  return {
    type: 'EXECUTE_QUERY',
    engineId,
    queryId,
    query,
  };
}

export function deleteQuery(queryId: string) {
  return {
    type: 'DELETE_QUERY',
    queryId,
  };
}

export function navigate(route: string) {
  return {
    type: 'NAVIGATE',
    route,
  };
}

export function moveTrack(trackId: string, direction: 'up'|'down') {
  return {
    type: 'MOVE_TRACK',
    trackId,
    direction,
  };
}

export function toggleTrackPinned(trackId: string) {
  return {
    type: 'TOGGLE_TRACK_PINNED',
    trackId,
  };
}

export function setEngineReady(engineId: string, ready = true) {
  return {type: 'SET_ENGINE_READY', engineId, ready};
}

export function createPermalink() {
  return {type: 'CREATE_PERMALINK', requestId: new Date().toISOString()};
}

export function setPermalink(requestId: string, hash: string) {
  return {type: 'SET_PERMALINK', requestId, hash};
}

export function loadPermalink(hash: string) {
  return {type: 'LOAD_PERMALINK', requestId: new Date().toISOString(), hash};
}

export function setState(newState: State) {
  return {
    type: 'SET_STATE',
    newState,
  };
}

export function setTraceTime(ts: TimeSpan) {
  return {
    type: 'SET_TRACE_TIME',
    startSec: ts.start,
    endSec: ts.end,
    lastUpdate: Date.now() / 1000,
  };
}

export function setVisibleTraceTime(ts: TimeSpan) {
  return {
    type: 'SET_VISIBLE_TRACE_TIME',
    startSec: ts.start,
    endSec: ts.end,
    lastUpdate: Date.now() / 1000,
  };
}

export function updateStatus(msg: string) {
  return {type: 'UPDATE_STATUS', msg, timestamp: Date.now() / 1000};
}
