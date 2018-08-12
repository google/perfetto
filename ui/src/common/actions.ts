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
export function addTrack(engineId: string, trackKind: string, cpu: number) {
  return {
    type: 'ADD_TRACK',
    engineId,
    trackKind,
    cpu,
  };
}

// TODO: There should be merged with addTrack above.
export function addChromeSliceTrack(
    engineId: string,
    trackKind: string,
    upid: number,
    utid: number,
    threadName: string,
    maxDepth: number) {
  return {
    type: 'ADD_CHROME_TRACK',
    engineId,
    trackKind,
    upid,
    utid,
    threadName,
    maxDepth,
  };
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

export function setEngineReady(engineId: string) {
  return {
    type: 'SET_ENGINE_READY',
    engineId,
  };
}

export function createPermalink() {
  return {
    type: 'CREATE_PERMALINK',
  };
}

export function setState(newState: State) {
  return {
    type: 'SET_STATE',
    newState,
  };
}

export function setTraceTime(startSec: number, endSec: number) {
  return {type: 'SET_TRACE_TIME', startSec, endSec};
}
