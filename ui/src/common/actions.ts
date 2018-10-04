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

import {defaultTraceTime, State, Status, TraceTime} from './state';

type StateDraft = DraftObject<State>;


function clearTraceState(state: StateDraft) {
  state.traceTime = defaultTraceTime;
  state.visibleTraceTime = defaultTraceTime;
  state.pinnedTracks = [];
  state.scrollingTracks = [];
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

  addTrack(
      state: StateDraft,
      args: {engineId: string; kind: string; name: string; config: {};}): void {
    const id = `${state.nextId++}`;
    state.tracks[id] = {
      id,
      engineId: args.engineId,
      kind: args.kind,
      name: args.name,
      config: args.config,
    };
    state.scrollingTracks.push(id);
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
      state: StateDraft, args: {trackId: string; direction: 'up' | 'down';}):
      void {
        const id = args.trackId;
        const isPinned = state.pinnedTracks.includes(id);
        const isScrolling = state.scrollingTracks.includes(id);
        if (!isScrolling && !isPinned) {
          throw new Error(`No track with id ${id}`);
        }
        const tracks = isPinned ? state.pinnedTracks : state.scrollingTracks;

        const oldIndex: number = tracks.indexOf(id);
        const newIndex = args.direction === 'up' ? oldIndex - 1 : oldIndex + 1;
        const swappedTrackId = tracks[newIndex];
        if (isPinned && newIndex === state.pinnedTracks.length) {
          // Move from last element of pinned to first element of scrolling.
          state.scrollingTracks.unshift(state.pinnedTracks.pop()!);
        } else if (isScrolling && newIndex === -1) {
          // Move first element of scrolling to last element of pinned.
          state.pinnedTracks.push(state.scrollingTracks.shift()!);
        } else if (swappedTrackId) {
          tracks[newIndex] = id;
          tracks[oldIndex] = swappedTrackId;
        }
      },

  toggleTrackPinned(state: StateDraft, args: {trackId: string}): void {
    const id = args.trackId;
    const isPinned = state.pinnedTracks.includes(id);

    if (isPinned) {
      state.pinnedTracks.splice(state.pinnedTracks.indexOf(id), 1);
      state.scrollingTracks.unshift(id);
    } else {
      state.scrollingTracks.splice(state.scrollingTracks.indexOf(id), 1);
      state.pinnedTracks.push(id);
    }
  },

  setEngineReady(state: StateDraft, args: {engineId: string; ready: boolean}):
      void {
        state.engines[args.engineId].ready = args.ready;
      },

  createPermalink(state: StateDraft, args: {requestId: string}): void {
    state.permalink = {requestId: args.requestId, hash: undefined};
  },

  setPermalink(state: StateDraft, args: {requestId: string; hash: string}):
      void {
        // Drop any links for old requests.
        if (state.permalink.requestId !== args.requestId) return;
        state.permalink = args;
      },

  loadPermalink(state: StateDraft, args: {requestId: string; hash: string}):
      void {
        state.permalink = args;
      },

  setTraceTime(state: StateDraft, args: TraceTime): void {
    state.traceTime = args;
  },

  setVisibleTraceTime(state: StateDraft, args: TraceTime): void {
    state.visibleTraceTime = args;
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
