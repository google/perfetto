// Copyright (C) 2023 The Android Open Source Project
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

import {Track, TrackDescriptor} from '../public';

import {TrackCache} from './track_cache';

function makeMockTrack(): Track {
  return {
    onCreate: jest.fn(),
    onUpdate: jest.fn(),
    onDestroy: jest.fn(),

    render: jest.fn(),
    onFullRedraw: jest.fn(),
    getSliceRect: jest.fn(),
    getHeight: jest.fn(),
    getTrackShellButtons: jest.fn(),
    onMouseMove: jest.fn(),
    onMouseClick: jest.fn(),
    onMouseOut: jest.fn(),
  };
}

async function settle() {
  await new Promise((r) => setTimeout(r, 0));
}

describe('TrackCache', () => {
  it('calls track lifecycle hooks', async () => {
    const track = makeMockTrack();
    const td: TrackDescriptor = {
      uri: 'test',
      track: () => track,
    };
    const trackCache = new TrackCache();

    const entry = trackCache.resolveTrack('foo', td);
    entry.update();
    await settle();
    expect(track.onUpdate).toHaveBeenCalledTimes(1);
  });

  it('reuses tracks', async () => {
    const track = makeMockTrack();
    const td: TrackDescriptor = {
      uri: 'test',
      track: () => track,
    };
    const trackCache = new TrackCache();

    const first = trackCache.resolveTrack('foo', td);
    trackCache.flushOldTracks();
    const second = trackCache.resolveTrack('foo', td);
    trackCache.flushOldTracks();

    // Ensure onCreate only called once
    expect(track.onCreate).toHaveBeenCalledTimes(1);
    expect(first).toBe(second);
  });

  it('destroys tracks', async () => {
    const track = makeMockTrack();
    const td: TrackDescriptor = {
      uri: 'test',
      track: () => track,
    };
    const trackCache = new TrackCache();

    trackCache.resolveTrack('foo', td);

    // Double flush should destroy all tracks
    trackCache.flushOldTracks();
    trackCache.flushOldTracks();

    await settle();

    // Ensure onCreate only called once
    expect(track.onDestroy).toHaveBeenCalledTimes(1);
  });
});

describe('TrackCacheEntry', () => {
  it('updates', async () => {
    const track = makeMockTrack();
    const td: TrackDescriptor = {
      uri: 'test',
      track: () => track,
    };
    const trackCache = new TrackCache();

    const entry = trackCache.resolveTrack('foo', td);
    entry.update();
    await settle();
    expect(track.onUpdate).toHaveBeenCalledTimes(1);
  });

  it('throws if updated when destroyed', async () => {
    const track = makeMockTrack();
    const td: TrackDescriptor = {
      uri: 'test',
      track: () => track,
    };
    const trackCache = new TrackCache();

    const entry = trackCache.resolveTrack('foo', td);

    // Double flush should destroy all tracks
    trackCache.flushOldTracks();
    trackCache.flushOldTracks();

    await settle();

    expect(() => entry.update()).toThrow();
  });
});
