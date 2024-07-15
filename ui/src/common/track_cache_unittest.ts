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

import {Duration} from '../base/time';
import {PxSpan, TimeScale} from '../frontend/time_scale';
import {createStore, TrackDescriptor} from '../public';
import {TrackRenderContext} from '../public/tracks';

import {createEmptyState} from './empty_state';
import {HighPrecisionTime} from './high_precision_time';
import {HighPrecisionTimeSpan} from './high_precision_time_span';
import {TrackManager} from './track_cache';

function makeMockTrack() {
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

let mockTrack: ReturnType<typeof makeMockTrack>;
let td: TrackDescriptor;
let trackManager: TrackManager;
const visibleWindow = new HighPrecisionTimeSpan(HighPrecisionTime.ZERO, 0);
const dummyCtx: TrackRenderContext = {
  trackKey: 'foo',
  ctx: new CanvasRenderingContext2D(),
  size: {width: 123, height: 123},
  visibleWindow,
  resolution: Duration.ZERO,
  timescale: new TimeScale(visibleWindow, new PxSpan(0, 0)),
};

beforeEach(() => {
  mockTrack = makeMockTrack();
  td = {
    uri: 'test',
    title: 'foo',
    trackFactory: () => mockTrack,
  };
  const store = createStore(createEmptyState());
  trackManager = new TrackManager(store);
});

describe('TrackManager', () => {
  it('calls track lifecycle hooks', async () => {
    const entry = trackManager.resolveTrack('foo', td);

    entry.render(dummyCtx);
    await settle();
    expect(mockTrack.onCreate).toHaveBeenCalledTimes(1);
    expect(mockTrack.onUpdate).toHaveBeenCalledTimes(1);

    entry[Symbol.dispose]();
    await settle();
    expect(mockTrack.onDestroy).toHaveBeenCalledTimes(1);
  });

  it('calls onCrate lazily', async () => {
    // Check we wait until the first call to render before calling onCreate
    const entry = trackManager.resolveTrack('foo', td);
    await settle();
    expect(mockTrack.onCreate).not.toHaveBeenCalled();

    entry.render(dummyCtx);
    await settle();
    expect(mockTrack.onCreate).toHaveBeenCalledTimes(1);
  });

  it('reuses tracks', async () => {
    const first = trackManager.resolveTrack('foo', td);
    trackManager.flushOldTracks();
    first.render(dummyCtx);
    await settle();

    const second = trackManager.resolveTrack('foo', td);
    trackManager.flushOldTracks();
    second.render(dummyCtx);
    await settle();

    expect(first).toBe(second);
    // Ensure onCreate called only once
    expect(mockTrack.onCreate).toHaveBeenCalledTimes(1);
  });

  it('destroys tracks when they are not resolved for one cycle', async () => {
    const entry = trackManager.resolveTrack('foo', td);
    entry.render(dummyCtx);

    // Double flush should destroy all tracks
    trackManager.flushOldTracks();
    trackManager.flushOldTracks();

    await settle();

    expect(mockTrack.onDestroy).toHaveBeenCalledTimes(1);
  });

  it('throws on render after destroy', async () => {
    const entry = trackManager.resolveTrack('foo', td);

    // Double flush should destroy all tracks
    trackManager.flushOldTracks();
    trackManager.flushOldTracks();

    await settle();

    expect(() => entry.render(dummyCtx)).toThrow();
  });

  it('contains crash inside onCreate()', async () => {
    const entry = trackManager.resolveTrack('foo', td);
    const e = new Error();

    // Mock crash inside onCreate
    mockTrack.onCreate.mockImplementationOnce(() => {
      throw e;
    });

    entry.render(dummyCtx);
    await settle();

    expect(mockTrack.onCreate).toHaveBeenCalledTimes(1);
    expect(mockTrack.onUpdate).not.toHaveBeenCalled();
    expect(mockTrack.onDestroy).toHaveBeenCalledTimes(1);
    expect(entry.getError()).toBe(e);
  });

  it('contains crash inside onUpdate()', async () => {
    const entry = trackManager.resolveTrack('foo', td);
    const e = new Error();

    // Mock crash inside onUpdate
    mockTrack.onUpdate.mockImplementationOnce(() => {
      throw e;
    });

    entry.render(dummyCtx);
    await settle();

    expect(mockTrack.onCreate).toHaveBeenCalledTimes(1);
    expect(mockTrack.onDestroy).toHaveBeenCalledTimes(1);
    expect(entry.getError()).toBe(e);
  });

  it('handles dispose after crash', async () => {
    const entry = trackManager.resolveTrack('foo', td);
    const e = new Error();

    // Mock crash inside onUpdate
    mockTrack.onUpdate.mockImplementationOnce(() => {
      throw e;
    });

    entry.render(dummyCtx);
    await settle();

    // Ensure we don't crash while disposing
    entry[Symbol.dispose]();
    await settle();
  });
});
