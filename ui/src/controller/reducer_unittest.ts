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

import {moveTrack, toggleTrackPinned} from '../common/actions';
import {createEmptyState, State, TrackState} from '../common/state';

import {rootReducer} from './reducer';

function fakeTrack(state: State, id: string): TrackState {
  const track: TrackState = {
    id,
    engineId: '1',
    maxDepth: 0,
    kind: 'SOME_TRACK_KIND',
    name: 'A track',
    cpu: 0,
  };
  state.tracks[id] = track;
  return track;
}

test('navigate', async () => {
  const before = createEmptyState();
  const after = rootReducer(before, {type: 'NAVIGATE', route: '/foo'});
  expect(after.route).toBe('/foo');
});

test('add tracks', () => {
  const empty = createEmptyState();
  const step1 = rootReducer(empty, {
    type: 'ADD_TRACK',
    engineId: '1',
    trackKind: 'cpu',
    cpu: '1',
  });
  const state = rootReducer(step1, {
    type: 'ADD_TRACK',
    engineId: '2',
    trackKind: 'cpu',
    cpu: '2',
  });
  expect(Object.values(state.tracks).length).toBe(2);
  expect(state.scrollingTracks.length).toBe(2);
});

test('reorder tracks', () => {
  const empty = createEmptyState();
  const step1 = rootReducer(empty, {
    type: 'ADD_TRACK',
    engineId: '1',
    trackKind: 'cpu',
    cpu: '1',
  });
  const before = rootReducer(step1, {
    type: 'ADD_TRACK',
    engineId: '2',
    trackKind: 'cpu',
    cpu: '2',
  });

  const firstTrackId = before.scrollingTracks[0];
  const secondTrackId = before.scrollingTracks[1];

  const after = rootReducer(before, {
    type: 'MOVE_TRACK',
    trackId: `${firstTrackId}`,
    direction: 'down',
  });

  // Ensure the order is swapped. This test would fail to detect side effects
  // if the before state was modified, so other tests are needed as well.
  expect(after.scrollingTracks[0]).toBe(secondTrackId);
  expect(after.scrollingTracks[1]).toBe(firstTrackId);

  // Ensure the track state contents have actually swapped places in the new
  // state, but not in the old one.
  expect(before.tracks[before.scrollingTracks[0]].engineId).toBe('1');
  expect(before.tracks[before.scrollingTracks[1]].engineId).toBe('2');
  expect(after.tracks[after.scrollingTracks[0]].engineId).toBe('2');
  expect(after.tracks[after.scrollingTracks[1]].engineId).toBe('1');
});

test('reorder pinned to scrolling', () => {
  const before = createEmptyState();

  fakeTrack(before, 'a');
  fakeTrack(before, 'b');
  fakeTrack(before, 'c');

  before.pinnedTracks = ['a', 'b'];
  before.scrollingTracks = ['c'];

  const after = rootReducer(before, moveTrack('b', 'down'));
  expect(after.pinnedTracks).toEqual(['a']);
  expect(after.scrollingTracks).toEqual(['b', 'c']);
});

test('reorder scrolling to pinned', () => {
  const before = createEmptyState();
  fakeTrack(before, 'a');
  fakeTrack(before, 'b');
  fakeTrack(before, 'c');

  before.pinnedTracks = ['a'];
  before.scrollingTracks = ['b', 'c'];

  const after = rootReducer(before, moveTrack('b', 'up'));
  expect(after.pinnedTracks).toEqual(['a', 'b']);
  expect(after.scrollingTracks).toEqual(['c']);
});

test('reorder clamp bottom', () => {
  const before = createEmptyState();
  fakeTrack(before, 'a');
  fakeTrack(before, 'b');
  fakeTrack(before, 'c');

  before.pinnedTracks = ['a', 'b'];
  before.scrollingTracks = ['c'];

  const after = rootReducer(before, moveTrack('a', 'up'));
  expect(after).toEqual(before);
});

test('reorder clamp top', () => {
  const before = createEmptyState();
  fakeTrack(before, 'a');
  fakeTrack(before, 'b');
  fakeTrack(before, 'c');

  before.pinnedTracks = ['a'];
  before.scrollingTracks = ['b', 'c'];

  const after = rootReducer(before, moveTrack('c', 'down'));
  expect(after).toEqual(before);
});

test('pin', () => {
  const before = createEmptyState();
  fakeTrack(before, 'a');
  fakeTrack(before, 'b');
  fakeTrack(before, 'c');

  before.pinnedTracks = ['a'];
  before.scrollingTracks = ['b', 'c'];

  const after = rootReducer(before, toggleTrackPinned('c'));
  expect(after.pinnedTracks).toEqual(['a', 'c']);
  expect(after.scrollingTracks).toEqual(['b']);
});

test('unpin', () => {
  const before = createEmptyState();
  fakeTrack(before, 'a');
  fakeTrack(before, 'b');
  fakeTrack(before, 'c');

  before.pinnedTracks = ['a', 'b'];
  before.scrollingTracks = ['c'];

  const after = rootReducer(before, toggleTrackPinned('a'));
  expect(after.pinnedTracks).toEqual(['b']);
  expect(after.scrollingTracks).toEqual(['a', 'c']);
});

test('open trace', async () => {
  const before = createEmptyState();
  const after = rootReducer(before, {
    type: 'OPEN_TRACE_FROM_URL',
    url: 'https://example.com/bar',
  });
  const engineKeys = Object.keys(after.engines);
  expect(engineKeys.length).toBe(1);
  expect(after.engines[engineKeys[0]].source).toBe('https://example.com/bar');
  expect(after.route).toBe('/viewer');
});

test('set state', async () => {
  const newState = createEmptyState();
  const before = createEmptyState();
  const after = rootReducer(before, {
    type: 'SET_STATE',
    newState,
  });
  expect(after).toBe(newState);
});
