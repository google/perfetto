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

import {PrimaryTrackSortKey} from '../public';

import {createEmptyState} from './empty_state';
import {getContainingGroupKey, State} from './state';

test('createEmptyState', () => {
  const state: State = createEmptyState();
  expect(state.engine).toEqual(undefined);
});

test('getContainingTrackId', () => {
  const state: State = createEmptyState();
  state.tracks['a'] = {
    key: 'a',
    uri: 'Foo',
    name: 'a track',
    trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
  };

  state.tracks['b'] = {
    key: 'b',
    uri: 'Foo',
    name: 'b track',
    trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
    trackGroup: 'containsB',
  };

  expect(getContainingGroupKey(state, 'z')).toEqual(null);
  expect(getContainingGroupKey(state, 'a')).toEqual(null);
  expect(getContainingGroupKey(state, 'b')).toEqual('containsB');
});
