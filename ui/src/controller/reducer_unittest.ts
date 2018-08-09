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

import {createEmptyState} from '../common/state';
import {rootReducer} from './reducer';

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
  expect(state.displayedTrackIds.length).toBe(2);
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

  const firstTrackId = before.displayedTrackIds[0];
  const secondTrackId = before.displayedTrackIds[1];

  const after = rootReducer(before, {
    type: 'MOVE_TRACK',
    trackId: `${firstTrackId}`,
    direction: 'down',
  });

  // Ensure the order is swapped. This test would fail to detect side effects
  // if the before state was modified, so other tests are needed as well.
  expect(after.displayedTrackIds[0]).toBe(secondTrackId);
  expect(after.displayedTrackIds[1]).toBe(firstTrackId);

  // Ensure the track state contents have actually swapped places in the new
  // state, but not in the old one.
  expect(before.tracks[before.displayedTrackIds[0]].engineId).toBe('1');
  expect(before.tracks[before.displayedTrackIds[1]].engineId).toBe('2');
  expect(after.tracks[after.displayedTrackIds[0]].engineId).toBe('2');
  expect(after.tracks[after.displayedTrackIds[1]].engineId).toBe('1');
});

test('open trace', async () => {
  const before = createEmptyState();
  const after = rootReducer(before, {
    type: 'OPEN_TRACE_FROM_URL',
    url: 'https://example.com/bar',
  });
  expect(after.engines[0].source).toBe('https://example.com/bar');
  expect(after.nextId).toBe(1);
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

test('create permalink', async () => {
  const before = createEmptyState();
  const after = rootReducer(before, {
    type: 'CREATE_PERMALINK',
  });
  expect(after.permalink!.state).toBe(before);
});
