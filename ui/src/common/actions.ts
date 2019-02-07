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

import {DraftObject} from 'immer';

import {assertExists} from '../base/logging';
import {ConvertTrace} from '../controller/trace_converter';

import {
  createEmptyState,
  RecordConfig,
  SCROLLING_TRACK_GROUP,
  State,
  Status,
  TraceTime,
} from './state';

type StateDraft = DraftObject<State>;


function clearTraceState(state: StateDraft) {
  const nextId = state.nextId;
  const recordConfig = state.recordConfig;
  const route = state.route;

  Object.assign(state, createEmptyState());
  state.nextId = nextId;
  state.recordConfig = recordConfig;
  state.route = route;
}

export const StateActions = {

  navigate(state: StateDraft, args: {route: string}): void {
    state.route = args.route;
  },

  openTraceFromFile(state: StateDraft, args: {file: File}): void {
    clearTraceState(state);
    const id = `${state.nextId++}`;
    state.engines[id] = {
      id,
      ready: false,
      source: args.file,
    };
    state.route = `/viewer`;
  },

  convertTraceToJson(_: StateDraft, args: {file: File}): void {
    ConvertTrace(args.file);
  },

  openTraceFromUrl(state: StateDraft, args: {url: string}): void {
    clearTraceState(state);
    const id = `${state.nextId++}`;
    state.engines[id] = {
      id,
      ready: false,
      source: args.url,
    };
    state.route = `/viewer`;
  },

  addTrack(state: StateDraft, args: {
    id?: string; engineId: string; kind: string; name: string;
    trackGroup?: string;
    config: {};
  }): void {
    const id = args.id !== undefined ? args.id : `${state.nextId++}`;
    state.tracks[id] = {
      id,
      engineId: args.engineId,
      kind: args.kind,
      name: args.name,
      trackGroup: args.trackGroup,
      config: args.config,
    };
    if (args.trackGroup === SCROLLING_TRACK_GROUP) {
      state.scrollingTracks.push(id);
    } else if (args.trackGroup !== undefined) {
      assertExists(state.trackGroups[args.trackGroup]).tracks.push(id);
    }
  },

  addTrackGroup(
      state: StateDraft,
      // Define ID in action so a track group can be referred to without running
      // the reducer.
      args: {
        engineId: string; name: string; id: string; summaryTrackId: string;
        collapsed: boolean;
      }): void {
    state.trackGroups[args.id] = {
      ...args,
      tracks: [],
    };
  },

  reqTrackData(state: StateDraft, args: {
    trackId: string; start: number; end: number; resolution: number;
  }): void {
    const id = args.trackId;
    state.tracks[id].dataReq = {
      start: args.start,
      end: args.end,
      resolution: args.resolution
    };
  },

  clearTrackDataReq(state: StateDraft, args: {trackId: string}): void {
    const id = args.trackId;
    state.tracks[id].dataReq = undefined;
  },

  executeQuery(
      state: StateDraft,
      args: {queryId: string; engineId: string; query: string}): void {
    state.queries[args.queryId] = {
      id: args.queryId,
      engineId: args.engineId,
      query: args.query,
    };
  },

  deleteQuery(state: StateDraft, args: {queryId: string}): void {
    delete state.queries[args.queryId];
  },

  moveTrack(
      state: StateDraft,
      args: {srcId: string; op: 'before' | 'after', dstId: string}): void {
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
      newList.forEach(x => {
        trackList.push(x);
      });
    };

    moveWithinTrackList(state.pinnedTracks);
    moveWithinTrackList(state.scrollingTracks);
  },

  toggleTrackPinned(state: StateDraft, args: {trackId: string}): void {
    const id = args.trackId;
    const isPinned = state.pinnedTracks.includes(id);
    const trackGroup = assertExists(state.tracks[id]).trackGroup;

    if (isPinned) {
      state.pinnedTracks.splice(state.pinnedTracks.indexOf(id), 1);
      if (trackGroup === SCROLLING_TRACK_GROUP) {
        state.scrollingTracks.unshift(id);
      }
    } else {
      if (trackGroup === SCROLLING_TRACK_GROUP) {
        state.scrollingTracks.splice(state.scrollingTracks.indexOf(id), 1);
      }
      state.pinnedTracks.push(id);
    }
  },

  toggleTrackGroupCollapsed(state: StateDraft, args: {trackGroupId: string}):
      void {
        const id = args.trackGroupId;
        const trackGroup = assertExists(state.trackGroups[id]);
        trackGroup.collapsed = !trackGroup.collapsed;
      },

  setEngineReady(state: StateDraft, args: {engineId: string; ready: boolean}):
      void {
        state.engines[args.engineId].ready = args.ready;
      },

  createPermalink(state: StateDraft, _: {}): void {
    state.permalink = {requestId: `${state.nextId++}`, hash: undefined};
  },

  setPermalink(state: StateDraft, args: {requestId: string; hash: string}):
      void {
        // Drop any links for old requests.
        if (state.permalink.requestId !== args.requestId) return;
        state.permalink = args;
      },

  loadPermalink(state: StateDraft, args: {hash: string}): void {
    state.permalink = {
      requestId: `${state.nextId++}`,
      hash: args.hash,
    };
  },

  clearPermalink(state: StateDraft, _: {}): void {
    state.permalink = {};
  },

  setTraceTime(state: StateDraft, args: TraceTime): void {
    state.traceTime = args;
  },

  setVisibleTraceTime(
      state: StateDraft, args: {time: TraceTime; lastUpdate: number;}): void {
    state.frontendLocalState.visibleTraceTime = args.time;
    state.frontendLocalState.lastUpdate = args.lastUpdate;
  },

  updateStatus(state: StateDraft, args: Status): void {
    state.status = args;
  },

  // TODO(hjd): Remove setState - it causes problems due to reuse of ids.
  setState(_state: StateDraft, _args: {newState: State}): void {
    // This has to be handled at a higher level since we can't
    // replace the whole tree here however we still need a method here
    // so it appears on the proxy Actions class.
    throw new Error('Called setState on StateActions.');
  },

  setConfig(state: StateDraft, args: {config: RecordConfig;}): void {
    state.recordConfig = args.config;
  },

  // TODO(hjd): Parametrize this to increase type safety. See comments on
  // aosp/778194
  setConfigControl(
      state: StateDraft,
      args: {name: string; value: string | number | boolean | null;}): void {
    const config = state.recordConfig;
    config[args.name] = args.value;
  },

  addConfigControl(
      state: StateDraft, args: {name: string; optionsToAdd: string[];}): void {
    // tslint:disable-next-line no-any
    const config = state.recordConfig as any;
    const options = config[args.name];
    for (const option of args.optionsToAdd) {
      if (options.includes(option)) continue;
      options.push(option);
    }
  },

  removeConfigControl(
      state: StateDraft, args: {name: string; optionsToRemove: string[];}):
      void {
        // tslint:disable-next-line no-any
        const config = state.recordConfig as any;
        const options = config[args.name];
        for (const option of args.optionsToRemove) {
          const index = options.indexOf(option);
          if (index === -1) continue;
          options.splice(index, 1);
        }
      },

  toggleDisplayConfigAsPbtxt(state: StateDraft, _: {}): void {
    state.displayConfigAsPbtxt = !state.displayConfigAsPbtxt;
  },

  selectNote(state: StateDraft, args: {id: string}): void {
    if (args.id) {
      state.currentSelection = {
        kind: 'NOTE',
        id: args.id
      };
    }
  },

  addNote(state: StateDraft, args: {timestamp: number}): void {
    const id = `${state.nextId++}`;
    state.notes[id] = {
      id,
      timestamp: args.timestamp,
      color: '#000000',
      text: '',
    };
    this.selectNote(state, {id});
  },

  changeNoteColor(state: StateDraft, args: {id: string, newColor: string}):
      void {
        const note = state.notes[args.id];
        if (note === undefined) return;
        note.color = args.newColor;
      },

  changeNoteText(state: StateDraft, args: {id: string, newText: string}): void {
    const note = state.notes[args.id];
    if (note === undefined) return;
    note.text = args.newText;
  },

  removeNote(state: StateDraft, args: {id: string}): void {
    delete state.notes[args.id];
    if (state.currentSelection === null) return;
    if (state.currentSelection.kind === 'NOTE' &&
        state.currentSelection.id === args.id) {
      state.currentSelection = null;
    }
  },

  selectSlice(state: StateDraft,
              args: {utid: number, id: number}): void {
    state.currentSelection = {
      kind: 'SLICE',
      utid: args.utid,
      id: args.id,
    };
  },

  deselect(state: StateDraft, _: {}): void {
    state.currentSelection = null;
  }

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
type DeferredActionFunc<T> = T extends ActionFunction<infer Args>?
    (args: Args) => DeferredAction<Args>:
    never;
type DeferredActions<C> = {
  [P in keyof C]: DeferredActionFunc<C[P]>;
};

// Actions is an implementation of DeferredActions<typeof StateActions>.
// (since StateActions is a variable not a type we have to do
// 'typeof StateActions' to access the (unnamed) type of StateActions).
// It's a Proxy such that any attribute access returns a function:
// (args) => {return {type: ATTRIBUTE_NAME, args};}
export const Actions =
    // tslint:disable-next-line no-any
    new Proxy<DeferredActions<typeof StateActions>>({} as any, {
      // tslint:disable-next-line no-any
      get(_: any, prop: string, _2: any) {
        return (args: {}): DeferredAction<{}> => {
          return {
            type: prop,
            args,
          };
        };
      },
    });
