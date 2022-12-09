// Copyright (C) 2021 The Android Open Source Project
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

import {createEmptyRecordConfig} from '../controller/record_config_types';
import {
  Aggregation,
} from '../frontend/pivot_table_redux_types';
import {
  autosaveConfigStore,
  recordTargetStore,
} from '../frontend/record_config';

import {featureFlags} from './feature_flags';
import {
  defaultTraceTime,
  NonSerializableState,
  State,
  STATE_VERSION,
} from './state';

const AUTOLOAD_STARTED_CONFIG_FLAG = featureFlags.register({
  id: 'autoloadStartedConfig',
  name: 'Auto-load last used recording config',
  description: 'Starting a recording automatically saves its configuration. ' +
      'This flag controls whether this config is automatically loaded.',
  defaultValue: true,
});

export function keyedMap<T>(
    keyFn: (key: T) => string, ...values: T[]): Map<string, T> {
  const result = new Map<string, T>();

  for (const value of values) {
    result.set(keyFn(value), value);
  }

  return result;
}

export const COUNT_AGGREGATION: Aggregation = {
  aggregationFunction: 'COUNT',
  // Exact column is ignored for count aggregation because it does not matter
  // what to count, use empty strings.
  column: {kind: 'regular', table: '', column: ''},
};

export function createEmptyNonSerializableState(): NonSerializableState {
  return {
    pivotTableRedux: {
      queryResult: null,
      selectedPivots: [{kind: 'regular', table: 'slice', column: 'name'}],
      selectedAggregations: [
        {
          aggregationFunction: 'SUM',
          column: {kind: 'regular', table: 'slice', column: 'dur'},
          sortDirection: 'DESC',
        },
        {
          aggregationFunction: 'SUM',
          column: {kind: 'regular', table: 'slice', column: 'thread_dur'},
        },
        COUNT_AGGREGATION,
      ],
      constrainToArea: true,
      queryRequested: false,
      argumentNames: [],
    },
  };
}

export function createEmptyState(): State {
  return {
    version: STATE_VERSION,
    nextId: '-1',
    newEngineMode: 'USE_HTTP_RPC_IF_AVAILABLE',
    traceTime: {...defaultTraceTime},
    tracks: {},
    uiTrackIdByTraceTrackId: {},
    utidToThreadSortKey: {},
    aggregatePreferences: {},
    trackGroups: {},
    visibleTracks: [],
    pinnedTracks: [],
    scrollingTracks: [],
    areas: {},
    queries: {},
    metrics: {},
    permalink: {},
    notes: {},
    visualisedArgs: [],

    recordConfig: AUTOLOAD_STARTED_CONFIG_FLAG.get() ?
        autosaveConfigStore.get() :
        createEmptyRecordConfig(),
    displayConfigAsPbtxt: false,
    lastLoadedConfig: {type: 'NONE'},

    frontendLocalState: {
      visibleState: {
        ...defaultTraceTime,
        lastUpdate: 0,
        resolution: 0,
      },
    },

    omniboxState: {
      omnibox: '',
      mode: 'SEARCH',
    },

    logsPagination: {
      offset: 0,
      count: 0,
    },

    status: {msg: '', timestamp: 0},
    currentSelection: null,
    currentFlamegraphState: null,
    traceConversionInProgress: false,

    perfDebug: false,
    sidebarVisible: true,
    hoveredUtid: -1,
    hoveredPid: -1,
    hoveredLogsTimestamp: -1,
    hoveredNoteTimestamp: -1,
    highlightedSliceId: -1,
    focusedFlowIdLeft: -1,
    focusedFlowIdRight: -1,
    searchIndex: -1,

    recordingInProgress: false,
    recordingCancelled: false,
    extensionInstalled: false,
    flamegraphModalDismissed: false,
    recordingTarget: recordTargetStore.getValidTarget(),
    availableAdbDevices: [],

    fetchChromeCategories: false,
    chromeCategories: undefined,
    nonSerializableState: createEmptyNonSerializableState(),

    logFilteringCriteria: {
      // The first two log priorities are ignored.
      minimumLevel: 2,
      tags: [],
      textEntry: '',
      hideNonMatching: true,
    },
  };
}
