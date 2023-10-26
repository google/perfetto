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
import {getContainingTrackId, State} from './state';
import {deserializeStateObject, serializeStateObject} from './upload_utils';

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

  expect(getContainingTrackId(state, 'z')).toEqual(null);
  expect(getContainingTrackId(state, 'a')).toEqual(null);
  expect(getContainingTrackId(state, 'b')).toEqual('containsB');
});

test('state is serializable', () => {
  const state: State = createEmptyState();
  const json = serializeStateObject(state);
  const restored: State = deserializeStateObject(json);

  // Remove nonSerializableState from original
  const serializableState: any = state as any;
  delete serializableState['nonSerializableState'];

  // Remove any undefined values from original as JSON doesn't serialize them
  for (const key in serializableState) {
    if (serializableState[key] === undefined) {
      delete serializableState[key];
    }
  }

  expect(serializableState).toEqual(restored);
});
