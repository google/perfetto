
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

import {TrackState} from '../common/state';
import {dingus} from '../test/dingus';

import {TrackCreator, TrackImpl} from './track_impl';
import {trackRegistry} from './track_registry';

// Cannot use dingus on an abstract class.
class MockTrackImpl extends TrackImpl {
  draw() {}
}

function mockTrackCreator(type: string): TrackCreator {
  return {
    type,
    create: () => new MockTrackImpl(dingus<TrackState>()),
  };
}

beforeEach(() => {
  trackRegistry.unregisterAllTracksForTesting();
});

test('trackRegistry returns correct track', () => {
  trackRegistry.register(mockTrackCreator('track1'));
  trackRegistry.register(mockTrackCreator('track2'));

  expect(trackRegistry.getCreator('track1').type).toEqual('track1');
  expect(trackRegistry.getCreator('track2').type).toEqual('track2');
});

test('trackRegistry throws error on name collision', () => {
  const creator1 = mockTrackCreator('someTrack');
  const creator2 = mockTrackCreator('someTrack');
  trackRegistry.register(creator1);
  expect(() => trackRegistry.register(creator2)).toThrow();
});

test('trackRegistry throws error on non-existent track', () => {
  expect(() => trackRegistry.getCreator('nonExistentTrack')).toThrow();
});
