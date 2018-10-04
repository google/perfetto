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

import {produce} from 'immer';
import {StateActions} from './actions';
import {createEmptyState, State, TrackState} from './state';

function fakeTrack(state: State, id: string): TrackState {
  const track: TrackState = {
    id,
    engineId: '1',
    kind: 'SOME_TRACK_KIND',
    name: 'A track',
    config: {},
  };
  state.tracks[id] = track;
  return track;
}

test('navigate', () => {
  const after = produce(createEmptyState(), draft => {
    StateActions.navigate(draft, {route: '/foo'});
  });
  expect(after.route).toBe('/foo');
});

test('add tracks', () => {
  const once = produce(createEmptyState(), draft => {
    StateActions.addTrack(draft, {
      engineId: '1',
      kind: 'cpu',
      name: 'Cpu 1',
      config: {},
    });
  });
  const twice = produce(once, draft => {
    StateActions.addTrack(draft, {
      engineId: '2',
      kind: 'cpu',
      name: 'Cpu 2',
      config: {},
    });
  });

  expect(Object.values(twice.tracks).length).toBe(2);
  expect(twice.scrollingTracks.length).toBe(2);
});

test('reorder tracks', () => {
  const once = produce(createEmptyState(), draft => {
    StateActions.addTrack(draft, {
      engineId: '1',
      kind: 'cpu',
      name: 'Cpu 1',
      config: {},
    });
    StateActions.addTrack(draft, {
      engineId: '2',
      kind: 'cpu',
      name: 'Cpu 2',
      config: {},
    });
  });

  const firstTrackId = once.scrollingTracks[0];
  const secondTrackId = once.scrollingTracks[1];

  const twice = produce(once, draft => {
    StateActions.moveTrack(draft, {
      trackId: `${firstTrackId}`,
      direction: 'down',
    });
  });

  expect(twice.scrollingTracks[0]).toBe(secondTrackId);
  expect(twice.scrollingTracks[1]).toBe(firstTrackId);
});

test('reorder pinned to scrolling', () => {
  const state = createEmptyState();
  fakeTrack(state, 'a');
  fakeTrack(state, 'b');
  fakeTrack(state, 'c');
  state.pinnedTracks = ['a', 'b'];
  state.scrollingTracks = ['c'];

  const after = produce(state, draft => {
    StateActions.moveTrack(draft, {
      trackId: 'b',
      direction: 'down',
    });
  });

  expect(after.pinnedTracks).toEqual(['a']);
  expect(after.scrollingTracks).toEqual(['b', 'c']);
});

test('reorder scrolling to pinned', () => {
  const state = createEmptyState();
  fakeTrack(state, 'a');
  fakeTrack(state, 'b');
  fakeTrack(state, 'c');
  state.pinnedTracks = ['a'];
  state.scrollingTracks = ['b', 'c'];

  const after = produce(state, draft => {
    StateActions.moveTrack(draft, {
      trackId: 'b',
      direction: 'up',
    });
  });

  expect(after.pinnedTracks).toEqual(['a', 'b']);
  expect(after.scrollingTracks).toEqual(['c']);
});

test('reorder clamp bottom', () => {
  const state = createEmptyState();
  fakeTrack(state, 'a');
  fakeTrack(state, 'b');
  fakeTrack(state, 'c');
  state.pinnedTracks = ['a', 'b'];
  state.scrollingTracks = ['c'];

  const after = produce(state, draft => {
    StateActions.moveTrack(draft, {
      trackId: 'a',
      direction: 'up',
    });
  });
  expect(after).toEqual(state);
});

test('reorder clamp top', () => {
  const state = createEmptyState();
  fakeTrack(state, 'a');
  fakeTrack(state, 'b');
  fakeTrack(state, 'c');
  state.pinnedTracks = ['a'];
  state.scrollingTracks = ['b', 'c'];

  const after = produce(state, draft => {
    StateActions.moveTrack(draft, {
      trackId: 'c',
      direction: 'down',
    });
  });
  expect(after).toEqual(state);
});

test('pin', () => {
  const state = createEmptyState();
  fakeTrack(state, 'a');
  fakeTrack(state, 'b');
  fakeTrack(state, 'c');
  state.pinnedTracks = ['a'];
  state.scrollingTracks = ['b', 'c'];

  const after = produce(state, draft => {
    StateActions.toggleTrackPinned(draft, {
      trackId: 'c',
    });
  });
  expect(after.pinnedTracks).toEqual(['a', 'c']);
  expect(after.scrollingTracks).toEqual(['b']);
});

test('unpin', () => {
  const state = createEmptyState();
  fakeTrack(state, 'a');
  fakeTrack(state, 'b');
  fakeTrack(state, 'c');
  state.pinnedTracks = ['a', 'b'];
  state.scrollingTracks = ['c'];

  const after = produce(state, draft => {
    StateActions.toggleTrackPinned(draft, {
      trackId: 'a',
    });
  });
  expect(after.pinnedTracks).toEqual(['b']);
  expect(after.scrollingTracks).toEqual(['a', 'c']);
});

test('open trace', () => {
  const after = produce(createEmptyState(), draft => {
    StateActions.openTraceFromUrl(draft, {
      url: 'https://example.com/bar',
    });
  });

  const engineKeys = Object.keys(after.engines);
  expect(engineKeys.length).toBe(1);
  expect(after.engines[engineKeys[0]].source).toBe('https://example.com/bar');
  expect(after.route).toBe('/viewer');
});

test('open second trace from file', () => {
  const once = produce(createEmptyState(), draft => {
    StateActions.openTraceFromUrl(draft, {
      url: 'https://example.com/bar',
    });
  });

  const twice = produce(once, draft => {
    StateActions.addTrack(draft, {
      engineId: '1',
      kind: 'cpu',
      name: 'Cpu 1',
      config: {},
    });
  });

  const thrice = produce(twice, draft => {
    StateActions.openTraceFromUrl(draft, {
      url: 'https://example.com/foo',
    });
  });

  const engineKeys = Object.keys(thrice.engines);
  expect(engineKeys.length).toBe(2);
  expect(thrice.engines[engineKeys[0]].source).toBe('https://example.com/bar');
  expect(thrice.engines[engineKeys[1]].source).toBe('https://example.com/foo');
  expect(thrice.pinnedTracks.length).toBe(0);
  expect(thrice.scrollingTracks.length).toBe(0);
  expect(thrice.route).toBe('/viewer');
});
