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

import {assertExists} from '../base/logging';
import {Duration} from '../base/time';
import {TimeScale} from '../base/time_scale';
import {TrackDescriptor, TrackRenderContext} from '../public/track';
import {HighPrecisionTime} from '../base/high_precision_time';
import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import {TrackManagerImpl} from '../core/track_manager';

function makeMockTrack() {
  return {
    onCreate: jest.fn(),
    onUpdate: jest.fn(),
    onDestroy: jest.fn(),

    render: jest.fn(),
    onFullRedraw: jest.fn(),
    getSliceVerticalBounds: jest.fn(),
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
let trackManager: TrackManagerImpl;
const visibleWindow = new HighPrecisionTimeSpan(HighPrecisionTime.ZERO, 0);
const dummyCtx: TrackRenderContext = {
  trackUri: 'foo',
  ctx: new CanvasRenderingContext2D(),
  size: {width: 123, height: 123},
  visibleWindow,
  resolution: Duration.ZERO,
  timescale: new TimeScale(visibleWindow, {left: 0, right: 0}),
};

beforeEach(() => {
  mockTrack = makeMockTrack();
  td = {
    uri: 'test',
    title: 'foo',
    track: mockTrack,
  };
  trackManager = new TrackManagerImpl();
  trackManager.registerTrack(td);
});

describe('TrackManager', () => {
  it('calls track lifecycle hooks', async () => {
    const entry = assertExists(trackManager.getTrackRenderer(td.uri));

    entry.render(dummyCtx);
    await settle();
    expect(mockTrack.onCreate).toHaveBeenCalledTimes(1);
    expect(mockTrack.onUpdate).toHaveBeenCalledTimes(1);

    // Double flush should destroy all tracks
    trackManager.flushOldTracks();
    trackManager.flushOldTracks();
    await settle();
    expect(mockTrack.onDestroy).toHaveBeenCalledTimes(1);
  });

  it('calls onCrate lazily', async () => {
    // Check we wait until the first call to render before calling onCreate
    const entry = assertExists(trackManager.getTrackRenderer(td.uri));
    await settle();
    expect(mockTrack.onCreate).not.toHaveBeenCalled();

    entry.render(dummyCtx);
    await settle();
    expect(mockTrack.onCreate).toHaveBeenCalledTimes(1);
  });

  it('reuses tracks', async () => {
    const first = assertExists(trackManager.getTrackRenderer(td.uri));
    trackManager.flushOldTracks();
    first.render(dummyCtx);
    await settle();

    const second = assertExists(trackManager.getTrackRenderer(td.uri));
    trackManager.flushOldTracks();
    second.render(dummyCtx);
    await settle();

    expect(first).toBe(second);
    // Ensure onCreate called only once
    expect(mockTrack.onCreate).toHaveBeenCalledTimes(1);
  });

  it('destroys tracks when they are not resolved for one cycle', async () => {
    const entry = assertExists(trackManager.getTrackRenderer(td.uri));
    entry.render(dummyCtx);

    // Double flush should destroy all tracks
    trackManager.flushOldTracks();
    trackManager.flushOldTracks();

    await settle();

    expect(mockTrack.onDestroy).toHaveBeenCalledTimes(1);
  });

  it('contains crash inside onCreate()', async () => {
    const entry = assertExists(trackManager.getTrackRenderer(td.uri));
    const e = new Error();

    // Mock crash inside onCreate
    mockTrack.onCreate.mockImplementationOnce(() => {
      throw e;
    });

    entry.render(dummyCtx);
    await settle();

    expect(mockTrack.onCreate).toHaveBeenCalledTimes(1);
    expect(mockTrack.onUpdate).not.toHaveBeenCalled();
    expect(entry.getError()).toBe(e);
  });

  it('contains crash inside onUpdate()', async () => {
    const entry = assertExists(trackManager.getTrackRenderer(td.uri));
    const e = new Error();

    // Mock crash inside onUpdate
    mockTrack.onUpdate.mockImplementationOnce(() => {
      throw e;
    });

    entry.render(dummyCtx);
    await settle();

    expect(mockTrack.onCreate).toHaveBeenCalledTimes(1);
    expect(mockTrack.onUpdate).toHaveBeenCalledTimes(1);
    expect(entry.getError()).toBe(e);
  });

  it('handles dispose after crash', async () => {
    const entry = assertExists(trackManager.getTrackRenderer(td.uri));
    const e = new Error();

    // Mock crash inside onUpdate
    mockTrack.onUpdate.mockImplementationOnce(() => {
      throw e;
    });

    entry.render(dummyCtx);
    await settle();

    // Ensure we don't crash during the next render cycle
    entry.render(dummyCtx);
    await settle();
  });
});
