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
import {assertExists, assertTrue} from '../base/logging';
import {duration, time} from '../base/time';
import {RecordConfig} from '../controller/record_config_types';
import {randomColor} from '../core/colorizer';
import {
  GenericSliceDetailsTabConfig,
  GenericSliceDetailsTabConfigBase,
} from '../frontend/generic_slice_details_tab';
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
//import {createEmptyState} from './empty_state';
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
  OmniboxMode,
  OmniboxState,
  PendingDeeplinkState,
  PivotTableResult,
  PrimaryTrackSortKey,
  ProfileType,
  RecordingTarget,
  SCROLLING_TRACK_GROUP,
  State,
  Status,
  ThreadTrackSortKey,
  TrackSortKey,
  UtidToTrackSortKey,
} from './state';

type StateDraft = Draft<State>;

export interface AddTrackArgs {
  key?: string;
  uri: string;
  name: string;
  trackSortKey: TrackSortKey;
  trackGroup?: string;
  closeable?: boolean;
}

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

/**
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
*/

function generateNextId(draft: StateDraft): string {
  const nextId = String(Number(draft.nextId) + 1);
  draft.nextId = nextId;
  return nextId;
}

// A helper to clean the state for a given removeable track.
// This is not exported as action to make it clear that not all
// tracks are removeable.
function removeTrack(state: StateDraft, trackKey: string) {
  const track = state.tracks[trackKey];
  if (track === undefined) {
    return;
  }
  delete state.tracks[trackKey];

  const removeTrackId = (arr: string[]) => {
    const index = arr.indexOf(trackKey);
    if (index !== -1) arr.splice(index, 1);
  };

  if (track.trackGroup === SCROLLING_TRACK_GROUP) {
    removeTrackId(state.scrollingTracks);
  } else if (track.trackGroup !== undefined) {
    const trackGroup = state.trackGroups[track.trackGroup];
    if (trackGroup !== undefined) {
      removeTrackId(trackGroup.tracks);
    }
  }
  state.pinnedTracks = state.pinnedTracks.filter((key) => key !== trackKey);
}

let statusTraceEvent: TraceEventScope | undefined;

export const StateActions = {
  openTraceFromFile(state: StateDraft, args: {file: File}): void {
    // clearTraceState(state);
    const id = generateNextId(state);
    state.engine = {
      id,
      ready: false,
      source: {type: 'FILE', file: args.file},
    };
  },

  openTraceFromBuffer(state: StateDraft, args: PostedTrace): void {
    // clearTraceState(state);
    const id = generateNextId(state);
    state.engine = {
      id,
      ready: false,
      source: {type: 'ARRAY_BUFFER', ...args},
    };
  },

  openTraceFromUrl(state: StateDraft, args: {url: string}): void {
    // clearTraceState(state);
    const id = generateNextId(state);
    state.engine = {
      id,
      ready: false,
      source: {type: 'URL', url: args.url},
    };
  },

  openTraceFromStoredFile(state: StateDraft, _args: {fileName: string}): void {
    // clearTraceState(state);
    const id = generateNextId(state);
    state.engine = {
      id,
      ready: false,
      source: {type: 'STORED_FILE', fileName: _args.fileName},
    };
  },

  openTraceFromHttpRpc(state: StateDraft, _args: {}): void {
    // clearTraceState(state);
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

  addTracks(state: StateDraft, args: {tracks: AddTrackArgs[]}) {
    args.tracks.forEach((track) => {
      const trackKey =
        track.key === undefined ? generateNextId(state) : track.key;
      const name = track.name;
      state.tracks[trackKey] = {
        key: trackKey,
        name,
        trackSortKey: track.trackSortKey,
        trackGroup: track.trackGroup,
        uri: track.uri,
        closeable: track.closeable,
      };
      if (track.trackGroup === SCROLLING_TRACK_GROUP) {
        state.scrollingTracks.push(trackKey);
      } else if (track.trackGroup !== undefined) {
        const group = state.trackGroups[track.trackGroup];
        if (group !== undefined) {
          group.tracks.push(trackKey);
        }
      }
    });
  },

  // Note: While this action has traditionally been omitted, with more and more
  // dynamic tracks being added and existing ones being moved to plugins, it
  // makes sense to have a generic "removeTracks" action which is un-opinionated
  // about what type of tracks we are removing.
  // E.g. Once debug tracks have been moved to a plugin, it makes no sense to
  // keep the "removeDebugTrack()" action, as the core should have no concept of
  // what debug tracks are.
  removeTracks(state: StateDraft, args: {trackKeys: string[]}) {
    for (const trackKey of args.trackKeys) {
      removeTrack(state, trackKey);
    }
  },

  setUtidToTrackSortKey(
    state: StateDraft,
    args: {threadOrderingMetadata: UtidToTrackSortKey},
  ) {
    state.utidToThreadSortKey = args.threadOrderingMetadata;
  },

  addTrack(state: StateDraft, args: AddTrackArgs): void {
    this.addTracks(state, {tracks: [args]});
  },

  addTrackGroup(
    state: StateDraft,
    // Define ID in action so a track group can be referred to without running
    // the reducer.
    args: {
      name: string;
      key: string;
      summaryTrackKey?: string;
      collapsed: boolean;
      fixedOrdering?: boolean;
    },
  ): void {
    state.trackGroups[args.key] = {
      name: args.name,
      key: args.key,
      collapsed: args.collapsed,
      tracks: [],
      summaryTrack: args.summaryTrackKey,
      fixedOrdering: args.fixedOrdering,
    };
  },

  maybeExpandOnlyTrackGroup(state: StateDraft, _: {}): void {
    const trackGroups = Object.values(state.trackGroups);
    if (trackGroups.length === 1) {
      trackGroups[0].collapsed = false;
    }
  },

  sortThreadTracks(state: StateDraft, _: {}) {
    const getFullKey = (a: string) => {
      const track = state.tracks[a];
      const threadTrackSortKey = track.trackSortKey as ThreadTrackSortKey;
      if (threadTrackSortKey.utid === undefined) {
        const sortKey = track.trackSortKey as PrimaryTrackSortKey;
        return [sortKey, 0, 0, 0];
      }
      const threadSortKey = state.utidToThreadSortKey[threadTrackSortKey.utid];
      return [
        /* eslint-disable @typescript-eslint/strict-boolean-expressions */
        threadSortKey
          ? threadSortKey.sortKey
          : PrimaryTrackSortKey.ORDINARY_THREAD,
        threadSortKey && threadSortKey.tid !== undefined
          ? threadSortKey.tid
          : Number.MAX_VALUE,
        /* eslint-enable */
        threadTrackSortKey.utid,
        threadTrackSortKey.priority,
      ];
    };

    // Use a numeric collator so threads are sorted as T1, T2, ..., T10, T11,
    // rather than T1, T10, T11, ..., T2, T20, T21 .
    const coll = new Intl.Collator([], {sensitivity: 'base', numeric: true});
    for (const group of Object.values(state.trackGroups)) {
      if (group.fixedOrdering) continue;

      group.tracks.sort((a: string, b: string) => {
        const aRank = getFullKey(a);
        const bRank = getFullKey(b);
        for (let i = 0; i < aRank.length; i++) {
          if (aRank[i] !== bRank[i]) return aRank[i] - bRank[i];
        }

        const aName = state.tracks[a].name.toLocaleLowerCase();
        const bName = state.tracks[b].name.toLocaleLowerCase();
        return coll.compare(aName, bName);
      });
    }
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

  moveTrack(
    state: StateDraft,
    args: {srcId: string; op: 'before' | 'after'; dstId: string},
  ): void {
    const moveWithinTrackList = (trackList: string[]) => {
      const newList: string[] = [];
      for (let i = 0; i < trackList.length; i++) {
        const curTrackId = trackList[i];
        if (curTrackId === args.dstId && args.op === 'before') {
          newList.push(args.srcId);
        }
        if (curTrackId !== args.srcId) {
          newList.push(curTrackId);
        }
        if (curTrackId === args.dstId && args.op === 'after') {
          newList.push(args.srcId);
        }
      }
      trackList.splice(0);
      newList.forEach((x) => {
        trackList.push(x);
      });
    };

    moveWithinTrackList(state.pinnedTracks);
    moveWithinTrackList(state.scrollingTracks);
  },

  toggleTrackPinned(state: StateDraft, args: {trackKey: string}): void {
    const key = args.trackKey;
    const isPinned = state.pinnedTracks.includes(key);
    const trackGroup = assertExists(state.tracks[key]).trackGroup;

    if (isPinned) {
      state.pinnedTracks.splice(state.pinnedTracks.indexOf(key), 1);
      if (trackGroup === SCROLLING_TRACK_GROUP) {
        state.scrollingTracks.unshift(key);
      }
    } else {
      if (trackGroup === SCROLLING_TRACK_GROUP) {
        state.scrollingTracks.splice(state.scrollingTracks.indexOf(key), 1);
      }
      state.pinnedTracks.push(key);
    }
  },

  toggleTrackGroupCollapsed(state: StateDraft, args: {groupKey: string}): void {
    const trackGroup = assertExists(state.trackGroups[args.groupKey]);
    trackGroup.collapsed = !trackGroup.collapsed;
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

  selectNote(state: StateDraft, args: {id: string}): void {
    state.selection = {
      kind: 'note',
      id: args.id,
    };
  },

  addNote(
    state: StateDraft,
    args: {timestamp: time; color: string; id?: string; text?: string},
  ): void {
    const {timestamp, color, id = generateNextId(state), text = ''} = args;
    state.notes[id] = {
      noteType: 'DEFAULT',
      id,
      timestamp,
      color,
      text,
    };
  },

  addSpanNote(
    state: StateDraft,
    args: {start: time; end: time; id?: string; color?: string},
  ): void {
    const {
      id = generateNextId(state),
      color = randomColor(),
      end,
      start,
    } = args;

    state.notes[id] = {
      noteType: 'SPAN',
      start,
      end,
      color,
      id,
      text: '',
    };
  },

  changeNoteColor(
    state: StateDraft,
    args: {id: string; newColor: string},
  ): void {
    const note = state.notes[args.id];
    if (note === undefined) return;
    note.color = args.newColor;
  },

  changeNoteText(state: StateDraft, args: {id: string; newText: string}): void {
    const note = state.notes[args.id];
    if (note === undefined) return;
    note.text = args.newText;
  },

  removeNote(state: StateDraft, args: {id: string}): void {
    delete state.notes[args.id];

    // Clear the selection if this note was selected
    if (state.selection.kind === 'note' && state.selection.id === args.id) {
      state.selection = {kind: 'empty'};
    }
  },

  selectHeapProfile(
    state: StateDraft,
    args: {id: number; upid: number; ts: time; type: ProfileType},
  ): void {
    state.selection = {
      kind: 'legacy',
      legacySelection: {
        kind: 'HEAP_PROFILE',
        id: args.id,
        upid: args.upid,
        ts: args.ts,
        type: args.type,
      },
    };
  },

  selectPerfSamples(
    state: StateDraft,
    args: {
      id: number;
      utid?: number;
      upid?: number;
      leftTs: time;
      rightTs: time;
      type: ProfileType;
    },
  ): void {
    state.selection = {
      kind: 'legacy',
      legacySelection: {
        kind: 'PERF_SAMPLES',
        id: args.id,
        utid: args.utid,
        upid: args.upid,
        leftTs: args.leftTs,
        rightTs: args.rightTs,
        type: args.type,
      },
    };
  },

  selectCpuProfileSample(
    state: StateDraft,
    args: {id: number; utid: number; ts: time},
  ): void {
    state.selection = {
      kind: 'legacy',
      legacySelection: {
        kind: 'CPU_PROFILE_SAMPLE',
        id: args.id,
        utid: args.utid,
        ts: args.ts,
      },
    };
  },

  selectSlice(
    state: StateDraft,
    args: {id: number; trackKey: string; table?: string; scroll?: boolean},
  ): void {
    state.selection = {
      kind: 'legacy',
      legacySelection: {
        kind: 'SLICE',
        id: args.id,
        trackKey: args.trackKey,
        table: args.table,
      },
    };
    state.pendingScrollId = args.scroll ? args.id : undefined;
  },

  selectGenericSlice(
    state: StateDraft,
    args: {
      id: number;
      sqlTableName: string;
      start: time;
      duration: duration;
      trackKey: string;
      detailsPanelConfig: {
        kind: string;
        config: GenericSliceDetailsTabConfigBase;
      };
    },
  ): void {
    const detailsPanelConfig: GenericSliceDetailsTabConfig = {
      id: args.id,
      ...args.detailsPanelConfig.config,
    };

    state.selection = {
      kind: 'legacy',
      legacySelection: {
        kind: 'GENERIC_SLICE',
        id: args.id,
        sqlTableName: args.sqlTableName,
        start: args.start,
        duration: args.duration,
        trackKey: args.trackKey,
        detailsPanelConfig: {
          kind: args.detailsPanelConfig.kind,
          config: detailsPanelConfig,
        },
      },
    };
  },

  setPendingScrollId(state: StateDraft, args: {pendingScrollId: number}): void {
    state.pendingScrollId = args.pendingScrollId;
  },

  clearPendingScrollId(state: StateDraft, _: {}): void {
    state.pendingScrollId = undefined;
  },

  selectThreadState(
    state: StateDraft,
    args: {id: number; trackKey: string},
  ): void {
    state.selection = {
      kind: 'legacy',
      legacySelection: {
        kind: 'THREAD_STATE',
        id: args.id,
        trackKey: args.trackKey,
      },
    };
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

  setOmnibox(state: StateDraft, args: OmniboxState): void {
    state.omniboxState = args;
  },

  setOmniboxMode(state: StateDraft, args: {mode: OmniboxMode}): void {
    state.omniboxState.mode = args.mode;
  },

  selectArea(
    state: StateDraft,
    args: {start: time; end: time; tracks: string[]},
  ): void {
    const {start, end, tracks} = args;
    assertTrue(start <= end);
    state.selection = {
      kind: 'area',
      start,
      end,
      tracks,
    };
  },

  toggleTrackSelection(
    state: StateDraft,
    args: {key: string; isTrackGroup: boolean},
  ) {
    const selection = state.selection;
    if (selection.kind !== 'area') {
      return;
    }

    const index = selection.tracks.indexOf(args.key);
    if (index > -1) {
      selection.tracks.splice(index, 1);
      if (args.isTrackGroup) {
        // Also remove all child tracks.
        for (const childTrack of state.trackGroups[args.key].tracks) {
          const childIndex = selection.tracks.indexOf(childTrack);
          if (childIndex > -1) {
            selection.tracks.splice(childIndex, 1);
          }
        }
      }
    } else {
      selection.tracks.push(args.key);
      if (args.isTrackGroup) {
        // Also add all child tracks.
        for (const childTrack of state.trackGroups[args.key].tracks) {
          if (!selection.tracks.includes(childTrack)) {
            selection.tracks.push(childTrack);
          }
        }
      }
    }
    // It's super unexpected that |toggleTrackSelection| does not cause
    // selection to be updated and this leads to bugs for people who do:
    // if (oldSelection !== state.selection) etc.
    // To solve this re-create the selection object here:
    state.selection = Object.assign({}, state.selection);
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

  setSearchIndex(state: StateDraft, args: {index: number}) {
    state.searchIndex = args.index;
  },

  setHoverCursorTimestamp(state: StateDraft, args: {ts: time}) {
    state.hoverCursorTimestamp = args.ts;
  },

  setHoveredNoteTimestamp(state: StateDraft, args: {ts: time}) {
    state.hoveredNoteTimestamp = args.ts;
  },

  // Add a tab with a given URI to the tab bar and show it.
  // If the tab is already present in the tab bar, just show it.
  showTab(state: StateDraft, args: {uri: string}) {
    // Add tab, unless we're talking about the special current_selection tab
    if (args.uri !== 'current_selection') {
      // Add tab to tab list if not already
      if (!state.tabs.openTabs.some((uri) => uri === args.uri)) {
        state.tabs.openTabs.push(args.uri);
      }
    }
    state.tabs.currentTab = args.uri;
  },

  // Hide a tab in the tab bar pick a new tab to show.
  // Note: Attempting to hide the "current_selection" tab doesn't work. This tab
  // is special and cannot be removed.
  hideTab(state: StateDraft, args: {uri: string}) {
    const tabs = state.tabs;
    // If the removed tab is the "current" tab, we must find a new tab to focus
    if (args.uri === tabs.currentTab) {
      // Remember the index of the current tab
      const currentTabIdx = tabs.openTabs.findIndex((uri) => uri === args.uri);

      // Remove the tab
      tabs.openTabs = tabs.openTabs.filter((uri) => uri !== args.uri);

      if (currentTabIdx !== -1) {
        if (tabs.openTabs.length === 0) {
          // No more tabs, use current selection
          tabs.currentTab = 'current_selection';
        } else if (currentTabIdx < tabs.openTabs.length - 1) {
          // Pick the tab to the right
          tabs.currentTab = tabs.openTabs[currentTabIdx];
        } else {
          // Pick the last tab
          const lastTab = tabs.openTabs[tabs.openTabs.length - 1];
          tabs.currentTab = lastTab;
        }
      }
    } else {
      // Otherwise just remove the tab
      tabs.openTabs = tabs.openTabs.filter((uri) => uri !== args.uri);
    }
  },

  clearAllPinnedTracks(state: StateDraft, _: {}) {
    const pinnedTracks = state.pinnedTracks.slice();
    for (let index = pinnedTracks.length - 1; index >= 0; index--) {
      const trackKey = pinnedTracks[index];
      this.toggleTrackPinned(state, {trackKey});
    }
  },

   clearAllTracks(state: StateDraft, _: {}) {
    // Clear all tracks from the state
    state.tracks = {};
    state.trackGroups = {};
    state.pinnedTracks = [];
    state.scrollingTracks = [];
  },

  togglePivotTable(
    state: StateDraft,
    args: {area?: {start: time; end: time; tracks: string[]}},
  ) {
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
