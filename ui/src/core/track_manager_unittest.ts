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
import {Track, TrackRenderContext} from '../public/track';
import {HighPrecisionTime} from '../base/high_precision_time';
import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import {TrackManagerImpl} from '../core/track_manager';
import {TrackNode} from '../public/workspace';
import {Renderer} from '../base/renderer';

interface MockTrack {
  render: jest.Mock;
  getSliceVerticalBounds: jest.Mock;
  getHeight: jest.Mock;
  getTrackShellButtons: jest.Mock;
  onMouseMove: jest.Mock;
  onMouseClick: jest.Mock;
  onMouseOut: jest.Mock;
}

function makeMockTrack(): MockTrack {
  return {
    render: jest.fn(),
    getSliceVerticalBounds: jest.fn(),
    getHeight: jest.fn(),
    getTrackShellButtons: jest.fn(),
    onMouseMove: jest.fn(),
    onMouseClick: jest.fn(),
    onMouseOut: jest.fn(),
  };
}

function makeMockRenderer(): Renderer {
  return {
    pushTransform: jest.fn().mockReturnValue({
      dispose: jest.fn(),
    }),
    clip: jest.fn().mockReturnValue({
      dispose: jest.fn(),
    }),
    drawMarkers: jest.fn(),
    drawRects: jest.fn(),
    drawStepArea: jest.fn(),
    resetTransform: jest.fn(),
    clear: jest.fn(),
  };
}

let mockTrack: ReturnType<typeof makeMockTrack>;
let td: Track;
let trackManager: TrackManagerImpl;
const visibleWindow = new HighPrecisionTimeSpan(HighPrecisionTime.ZERO, 0);
const dummyTrackNode = new TrackNode({name: 'test', uri: 'foo'});
const dummyCtx: TrackRenderContext = {
  trackUri: 'foo',
  trackNode: dummyTrackNode,
  ctx: new CanvasRenderingContext2D(),
  size: {width: 123, height: 123},
  visibleWindow,
  resolution: Duration.ZERO,
  timescale: new TimeScale(visibleWindow, {left: 0, right: 0}),
  colors: {
    COLOR_BORDER: 'hotpink',
    COLOR_BORDER_SECONDARY: 'hotpink',
    COLOR_BACKGROUND_SECONDARY: 'hotpink',
    COLOR_ACCENT: 'hotpink',
    COLOR_BACKGROUND: 'hotpink',
    COLOR_TEXT: 'hotpink',
    COLOR_TEXT_MUTED: 'hotpink',
    COLOR_NEUTRAL: 'hotpink',
    COLOR_TIMELINE_OVERLAY: 'hotpink',
  },
  renderer: makeMockRenderer(),
};

beforeEach(() => {
  mockTrack = makeMockTrack();
  td = {
    uri: 'test',
    renderer: mockTrack,
  };
  trackManager = new TrackManagerImpl();
  trackManager.registerTrack(td);
});

describe('TrackManager', () => {
  it('calls render on the track', () => {
    const entry = assertExists(trackManager.getWrappedTrack(td.uri));

    entry.render(dummyCtx);
    expect(mockTrack.render).toHaveBeenCalledTimes(1);
    expect(mockTrack.render).toHaveBeenCalledWith(dummyCtx);
  });

  it('reuses tracks across render cycles', () => {
    const first = assertExists(trackManager.getWrappedTrack(td.uri));
    first.render(dummyCtx);

    const second = assertExists(trackManager.getWrappedTrack(td.uri));
    second.render(dummyCtx);

    expect(first).toBe(second);
    expect(mockTrack.render).toHaveBeenCalledTimes(2);
  });

  it('contains crash inside render()', () => {
    const entry = assertExists(trackManager.getWrappedTrack(td.uri));
    const e = new Error('test error');

    // Mock crash inside render
    mockTrack.render.mockImplementationOnce(() => {
      throw e;
    });

    entry.render(dummyCtx);

    expect(mockTrack.render).toHaveBeenCalledTimes(1);
    expect(entry.getError()).toBe(e);
  });

  it('does not call render after crash', () => {
    const entry = assertExists(trackManager.getWrappedTrack(td.uri));
    const e = new Error('test error');

    // Mock crash inside render
    mockTrack.render.mockImplementationOnce(() => {
      throw e;
    });

    entry.render(dummyCtx);
    expect(entry.getError()).toBe(e);

    // Subsequent renders should be no-ops
    entry.render(dummyCtx);
    expect(mockTrack.render).toHaveBeenCalledTimes(1);
  });

  it('exposes the track renderer', () => {
    const entry = assertExists(trackManager.getWrappedTrack(td.uri));
    expect(entry.track).toBe(mockTrack);
  });

  it('exposes the track descriptor', () => {
    const entry = assertExists(trackManager.getWrappedTrack(td.uri));
    expect(entry.desc).toBe(td);
  });
});
