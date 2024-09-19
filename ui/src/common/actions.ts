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

import {Draft} from 'immer';
import {SortDirection} from '../base/comparison_utils';
import {time} from '../base/time';
import {RecordConfig} from '../controller/record_config_types';
import {
  Aggregation,
  AggregationFunction,
  TableColumn,
  tableColumnEquals,
  toggleEnabled,
} from '../frontend/pivot_table_types';
import {
  computeIntervals,
  DropDirection,
  performReordering,
} from './dragndrop_logic';
import {createEmptyState} from './empty_state';
import {
  MetatraceTrackId,
  traceEventBegin,
  traceEventEnd,
  TraceEventScope,
} from './metatracing';
import {
  AdbRecordingTarget,
  EngineMode,
  LoadedConfig,
  NewEngineMode,
  PendingDeeplinkState,
  PivotTableResult,
  RecordingTarget,
  State,
  Status,
} from './state';
import {Area} from '../public/selection';

type StateDraft = Draft<State>;

export interface PostedTrace {
  buffer: ArrayBuffer;
  title: string;
  fileName?: string;
  url?: string;
  uuid?: string;
  localOnly?: boolean;
  keepApiOpen?: boolean;

  // Allows to pass extra arguments to plugins. This can be read by plugins
  // onTraceLoad() and can be used to trigger plugin-specific-behaviours (e.g.
  // allow dashboards like APC to pass extra data to materialize onto tracks).
  // The format is the following:
  // pluginArgs: {
  //   'dev.perfetto.PluginFoo': { 'key1': 'value1', 'key2': 1234 }
  //   'dev.perfetto.PluginBar': { 'key3': '...', 'key4': ... }
  // }
  pluginArgs?: {[pluginId: string]: {[key: string]: unknown}};
}

export interface PostedScrollToRange {
  timeStart: number;
  timeEnd: number;
  viewPercentage?: number;
}

function clearTraceState(state: StateDraft) {
  const nextId = state.nextId;
  const recordConfig = state.recordConfig;
  const recordingTarget = state.recordingTarget;
  const fetchChromeCategories = state.fetchChromeCategories;
  const extensionInstalled = state.extensionInstalled;
  const availableAdbDevices = state.availableAdbDevices;
  const chromeCategories = state.chromeCategories;
  const newEngineMode = state.newEngineMode;

  Object.assign(state, createEmptyState());
  state.nextId = nextId;
  state.recordConfig = recordConfig;
  state.recordingTarget = recordingTarget;
  state.fetchChromeCategories = fetchChromeCategories;
  state.extensionInstalled = extensionInstalled;
  state.availableAdbDevices = availableAdbDevices;
  state.chromeCategories = chromeCategories;
  state.newEngineMode = newEngineMode;
}

function generateNextId(draft: StateDraft): string {
  const nextId = String(Number(draft.nextId) + 1);
  draft.nextId = nextId;
  return nextId;
}

let statusTraceEvent: TraceEventScope | undefined;

export const StateActions = {
  openTraceFromFile(state: StateDraft, args: {file: File}): void {
    clearTraceState(state);
    const id = generateNextId(state);
    state.engine = {
      id,
      ready: false,
      source: {type: 'FILE', file: args.file},
    };
  },

  openTraceFromBuffer(state: StateDraft, args: PostedTrace): void {
    clearTraceState(state);
    const id = generateNextId(state);
    state.engine = {
      id,
      ready: false,
      source: {type: 'ARRAY_BUFFER', ...args},
    };
  },

  openTraceFromUrl(state: StateDraft, args: {url: string}): void {
    clearTraceState(state);
    const id = generateNextId(state);
    state.engine = {
      id,
      ready: false,
      source: {type: 'URL', url: args.url},
    };
  },

  openTraceFromHttpRpc(state: StateDraft, _args: {}): void {
    clearTraceState(state);
    const id = generateNextId(state);
    state.engine = {
      id,
      ready: false,
      source: {type: 'HTTP_RPC'},
    };
  },

  setTraceUuid(state: StateDraft, args: {traceUuid: string}) {
    state.traceUuid = args.traceUuid;
  },

  updateAggregateSorting(
    state: StateDraft,
    args: {id: string; column: string},
  ) {
    let prefs = state.aggregatePreferences[args.id];
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    if (!prefs) {
      prefs = {id: args.id};
      state.aggregatePreferences[args.id] = prefs;
    }

    if (!prefs.sorting || prefs.sorting.column !== args.column) {
      // No sorting set for current column.
      state.aggregatePreferences[args.id].sorting = {
        column: args.column,
        direction: 'DESC',
      };
    } else if (prefs.sorting.direction === 'DESC') {
      // Toggle the direction if the column is currently sorted.
      state.aggregatePreferences[args.id].sorting = {
        column: args.column,
        direction: 'ASC',
      };
    } else {
      // If direction is currently 'ASC' toggle to no sorting.
      state.aggregatePreferences[args.id].sorting = undefined;
    }
  },

  requestTrackReload(state: StateDraft, _: {}) {
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    if (state.lastTrackReloadRequest) {
      state.lastTrackReloadRequest++;
    } else {
      state.lastTrackReloadRequest = 1;
    }
  },

  maybeSetPendingDeeplink(state: StateDraft, args: PendingDeeplinkState) {
    state.pendingDeeplink = args;
  },

  clearPendingDeeplink(state: StateDraft, _: {}) {
    state.pendingDeeplink = undefined;
  },

  // TODO(hjd): engine.ready should be a published thing. If it's part
  // of the state it interacts badly with permalinks.
  setEngineReady(
    state: StateDraft,
    args: {engineId: string; ready: boolean; mode: EngineMode},
  ): void {
    const engine = state.engine;
    if (engine === undefined || engine.id !== args.engineId) {
      return;
    }
    engine.ready = args.ready;
    engine.mode = args.mode;
  },

  setNewEngineMode(state: StateDraft, args: {mode: NewEngineMode}): void {
    state.newEngineMode = args.mode;
  },

  // Marks all engines matching the given |mode| as failed.
  setEngineFailed(
    state: StateDraft,
    args: {mode: EngineMode; failure: string},
  ): void {
    if (state.engine !== undefined && state.engine.mode === args.mode) {
      state.engine.failed = args.failure;
    }
  },

  updateStatus(state: StateDraft, args: Status): void {
    if (statusTraceEvent) {
      traceEventEnd(statusTraceEvent);
    }
    statusTraceEvent = traceEventBegin(args.msg, {
      track: MetatraceTrackId.kOmniboxStatus,
    });
    state.status = args;
  },

  // TODO(hjd): Remove setState - it causes problems due to reuse of ids.
  setState(state: StateDraft, args: {newState: State}): void {
    for (const key of Object.keys(state)) {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      delete (state as any)[key];
    }
    for (const key of Object.keys(args.newState)) {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      (state as any)[key] = (args.newState as any)[key];
    }

    // If we're loading from a permalink then none of the engines can
    // possibly be ready:
    if (state.engine !== undefined) {
      state.engine.ready = false;
    }
  },

  setRecordConfig(
    state: StateDraft,
    args: {config: RecordConfig; configType?: LoadedConfig},
  ): void {
    state.recordConfig = args.config;
    state.lastLoadedConfig = args.configType || {type: 'NONE'};
  },

  startRecording(state: StateDraft, _: {}): void {
    state.recordingInProgress = true;
    state.lastRecordingError = undefined;
    state.recordingCancelled = false;
  },

  stopRecording(state: StateDraft, _: {}): void {
    state.recordingInProgress = false;
  },

  cancelRecording(state: StateDraft, _: {}): void {
    state.recordingInProgress = false;
    state.recordingCancelled = true;
  },

  setExtensionAvailable(state: StateDraft, args: {available: boolean}): void {
    state.extensionInstalled = args.available;
  },

  setRecordingTarget(state: StateDraft, args: {target: RecordingTarget}): void {
    state.recordingTarget = args.target;
  },

  setFetchChromeCategories(state: StateDraft, args: {fetch: boolean}): void {
    state.fetchChromeCategories = args.fetch;
  },

  setAvailableAdbDevices(
    state: StateDraft,
    args: {devices: AdbRecordingTarget[]},
  ): void {
    state.availableAdbDevices = args.devices;
  },

  setChromeCategories(state: StateDraft, args: {categories: string[]}): void {
    state.chromeCategories = args.categories;
  },

  setLastRecordingError(state: StateDraft, args: {error?: string}): void {
    state.lastRecordingError = args.error;
    state.recordingStatus = undefined;
  },

  setRecordingStatus(state: StateDraft, args: {status?: string}): void {
    state.recordingStatus = args.status;
    state.lastRecordingError = undefined;
  },

  togglePerfDebug(state: StateDraft, _: {}): void {
    state.perfDebug = !state.perfDebug;
  },

  setSidebar(state: StateDraft, args: {visible: boolean}): void {
    state.sidebarVisible = args.visible;
  },

  setHoveredUtidAndPid(state: StateDraft, args: {utid: number; pid: number}) {
    state.hoveredPid = args.pid;
    state.hoveredUtid = args.utid;
  },

  setHighlightedSliceId(state: StateDraft, args: {sliceId: number}) {
    state.highlightedSliceId = args.sliceId;
  },

  setHighlightedFlowLeftId(state: StateDraft, args: {flowId: number}) {
    state.focusedFlowIdLeft = args.flowId;
  },

  setHighlightedFlowRightId(state: StateDraft, args: {flowId: number}) {
    state.focusedFlowIdRight = args.flowId;
  },

  setHoverCursorTimestamp(state: StateDraft, args: {ts: time}) {
    state.hoverCursorTimestamp = args.ts;
  },

  setHoveredNoteTimestamp(state: StateDraft, args: {ts: time}) {
    state.hoveredNoteTimestamp = args.ts;
  },

  togglePivotTable(state: StateDraft, args: {area?: Area}) {
    state.nonSerializableState.pivotTable.selectionArea = args.area;
    state.nonSerializableState.pivotTable.queryResult = null;
  },

  setPivotStateQueryResult(
    state: StateDraft,
    args: {queryResult: PivotTableResult | null},
  ) {
    state.nonSerializableState.pivotTable.queryResult = args.queryResult;
  },

  setPivotTableConstrainToArea(state: StateDraft, args: {constrain: boolean}) {
    state.nonSerializableState.pivotTable.constrainToArea = args.constrain;
  },

  dismissFlamegraphModal(state: StateDraft, _: {}) {
    state.flamegraphModalDismissed = true;
  },

  addPivotTableAggregation(
    state: StateDraft,
    args: {aggregation: Aggregation; after: number},
  ) {
    state.nonSerializableState.pivotTable.selectedAggregations.splice(
      args.after,
      0,
      args.aggregation,
    );
  },

  removePivotTableAggregation(state: StateDraft, args: {index: number}) {
    state.nonSerializableState.pivotTable.selectedAggregations.splice(
      args.index,
      1,
    );
  },

  setPivotTableQueryRequested(
    state: StateDraft,
    args: {queryRequested: boolean},
  ) {
    state.nonSerializableState.pivotTable.queryRequested = args.queryRequested;
  },

  setPivotTablePivotSelected(
    state: StateDraft,
    args: {column: TableColumn; selected: boolean},
  ) {
    toggleEnabled(
      tableColumnEquals,
      state.nonSerializableState.pivotTable.selectedPivots,
      args.column,
      args.selected,
    );
  },

  setPivotTableAggregationFunction(
    state: StateDraft,
    args: {index: number; function: AggregationFunction},
  ) {
    state.nonSerializableState.pivotTable.selectedAggregations[
      args.index
    ].aggregationFunction = args.function;
  },

  setPivotTableSortColumn(
    state: StateDraft,
    args: {aggregationIndex: number; order: SortDirection},
  ) {
    state.nonSerializableState.pivotTable.selectedAggregations =
      state.nonSerializableState.pivotTable.selectedAggregations.map(
        (agg, index) => ({
          column: agg.column,
          aggregationFunction: agg.aggregationFunction,
          sortDirection:
            index === args.aggregationIndex ? args.order : undefined,
        }),
      );
  },

  changePivotTablePivotOrder(
    state: StateDraft,
    args: {from: number; to: number; direction: DropDirection},
  ) {
    const pivots = state.nonSerializableState.pivotTable.selectedPivots;
    state.nonSerializableState.pivotTable.selectedPivots = performReordering(
      computeIntervals(pivots.length, args.from, args.to, args.direction),
      pivots,
    );
  },

  changePivotTableAggregationOrder(
    state: StateDraft,
    args: {from: number; to: number; direction: DropDirection},
  ) {
    const aggregations =
      state.nonSerializableState.pivotTable.selectedAggregations;
    state.nonSerializableState.pivotTable.selectedAggregations =
      performReordering(
        computeIntervals(
          aggregations.length,
          args.from,
          args.to,
          args.direction,
        ),
        aggregations,
      );
  },

  setTrackFilterTerm(
    state: StateDraft,
    args: {filterTerm: string | undefined},
  ) {
    state.trackFilterTerm = args.filterTerm;
  },

  runControllers(state: StateDraft, _args: {}) {
    state.forceRunControllers++;
  },
};

// When we are on the frontend side, we don't really want to execute the
// actions above, we just want to serialize them and marshal their
// arguments, send them over to the controller side and have them being
// executed there. The magic below takes care of turning each action into a
// function that returns the marshaled args.

// A DeferredAction is a bundle of Args and a method name. This is the marshaled
// version of a StateActions method call.
export interface DeferredAction<Args = {}> {
  type: string;
  args: Args;
}

// This type magic creates a type function DeferredActions<T> which takes a type
// T and 'maps' its attributes. For each attribute on T matching the signature:
// (state: StateDraft, args: Args) => void
// DeferredActions<T> has an attribute:
// (args: Args) => DeferredAction<Args>
type ActionFunction<Args> = (state: StateDraft, args: Args) => void;
type DeferredActionFunc<T> =
  T extends ActionFunction<infer Args>
    ? (args: Args) => DeferredAction<Args>
    : never;
type DeferredActions<C> = {
  [P in keyof C]: DeferredActionFunc<C[P]>;
};

// Actions is an implementation of DeferredActions<typeof StateActions>.
// (since StateActions is a variable not a type we have to do
// 'typeof StateActions' to access the (unnamed) type of StateActions).
// It's a Proxy such that any attribute access returns a function:
// (args) => {return {type: ATTRIBUTE_NAME, args};}
export const Actions =
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  new Proxy<DeferredActions<typeof StateActions>>({} as any, {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    get(_: any, prop: string, _2: any) {
      return (args: {}): DeferredAction<{}> => {
        return {
          type: prop,
          args,
        };
      };
    },
  });
