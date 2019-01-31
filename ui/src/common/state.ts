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

/**
 * A plain js object, holding objects of type |Class| keyed by string id.
 * We use this instead of using |Map| object since it is simpler and faster to
 * serialize for use in postMessage.
 */
export interface ObjectById<Class extends{id: string}> { [id: string]: Class; }

export const SCROLLING_TRACK_GROUP = 'ScrollingTracks';

export interface TrackState {
  id: string;
  engineId: string;
  kind: string;
  name: string;
  trackGroup?: string;
  dataReq?: TrackDataRequest;
  config: {};
}

export interface TrackGroupState {
  id: string;
  engineId: string;
  name: string;
  collapsed: boolean;
  tracks: string[];  // Child track ids.
  summaryTrackId: string;
}

export interface TrackDataRequest {
  start: number;
  end: number;
  resolution: number;
}

export interface EngineConfig {
  id: string;
  ready: boolean;
  source: string|File;
}

export interface QueryConfig {
  id: string;
  engineId: string;
  query: string;
}

export interface PermalinkConfig {
  requestId?: string;  // Set by the frontend to request a new permalink.
  hash?: string;       // Set by the controller when the link has been created.
}

export interface RecordConfig {
  [key: string]: null|number|boolean|string|string[];

  // Global settings
  durationSeconds: number;
  writeIntoFile: boolean;
  fileWritePeriodMs: number|null;

  // Buffer setup
  bufferSizeMb: number;

  // Ftrace
  ftrace: boolean;
  ftraceEvents: string[];
  atraceCategories: string[];
  atraceApps: string[];
  ftraceDrainPeriodMs: number|null;
  ftraceBufferSizeKb: number|null;

  // Ps
  processMetadata: boolean;
  scanAllProcessesOnStart: boolean;
  procStatusPeriodMs: number|null;

  // SysStats
  sysStats: boolean;
  meminfoPeriodMs: number|null;
  meminfoCounters: string[];
  vmstatPeriodMs: number|null;
  vmstatCounters: string[];
  statPeriodMs: number|null;
  statCounters: string[];

  // Battery and power
  power: boolean;
  batteryPeriodMs: number|null;
  batteryCounters: string[];
}

export interface TraceTime {
  startSec: number;
  endSec: number;
}

export interface FrontendLocalState {
  visibleTraceTime: TraceTime;
  lastUpdate: number;  // Epoch in seconds (Date.now() / 1000).
}

export interface Status {
  msg: string;
  timestamp: number;  // Epoch in seconds (Date.now() / 1000).
}

export interface Note {
  id: string;
  timestamp: number;
  color: string;
  text: string;
}

export interface SliceSelection {
  utid: number;
  id: number;
}

export interface State {
  route: string|null;
  nextId: number;

  /**
   * State of the ConfigEditor.
   */
  recordConfig: RecordConfig;
  displayConfigAsPbtxt: boolean;

  /**
   * Open traces.
   */
  engines: ObjectById<EngineConfig>;
  traceTime: TraceTime;
  trackGroups: ObjectById<TrackGroupState>;
  tracks: ObjectById<TrackState>;
  scrollingTracks: string[];
  pinnedTracks: string[];
  queries: ObjectById<QueryConfig>;
  permalink: PermalinkConfig;
  notes: ObjectById<Note>;
  status: Status;
  selectedNote: string|null;
  selectedSlice: SliceSelection|null;

  /**
   * This state is updated on the frontend at 60Hz and eventually syncronised to
   * the controller at 10Hz. When the controller sends state updates to the
   * frontend the frontend has special logic to pick whichever version of this
   * key is most up to date.
   */
  frontendLocalState: FrontendLocalState;
}

export const defaultTraceTime = {
  startSec: 0,
  endSec: 10,
};

export function createEmptyState(): State {
  return {
    route: null,
    nextId: 0,
    engines: {},
    traceTime: {...defaultTraceTime},
    tracks: {},
    trackGroups: {},
    pinnedTracks: [],
    scrollingTracks: [],
    queries: {},
    permalink: {},
    notes: {},

    recordConfig: createEmptyRecordConfig(),
    displayConfigAsPbtxt: false,

    frontendLocalState: {
      visibleTraceTime: {...defaultTraceTime},
      lastUpdate: 0,
    },

    status: {msg: '', timestamp: 0},
    selectedNote: null,
    selectedSlice: null,
  };
}

export function createEmptyRecordConfig(): RecordConfig {
  return {
    durationSeconds: 10.0,
    writeIntoFile: false,
    fileWritePeriodMs: null,
    bufferSizeMb: 10.0,

    ftrace: false,
    ftraceEvents: [],
    atraceApps: [],
    atraceCategories: [],
    ftraceDrainPeriodMs: null,
    ftraceBufferSizeKb: 2 * 1024,

    processMetadata: false,
    scanAllProcessesOnStart: false,
    procStatusPeriodMs: null,

    sysStats: false,
    meminfoPeriodMs: null,
    meminfoCounters: [],
    vmstatPeriodMs: null,
    vmstatCounters: [],
    statPeriodMs: null,
    statCounters: [],

    power: false,
    batteryPeriodMs: 1000,
    batteryCounters: [],
  };
}
