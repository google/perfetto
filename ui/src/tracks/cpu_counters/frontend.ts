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

import {TrackState} from '../../common/state';
import {TimeScale} from '../../frontend/time_scale';
import {Track} from '../../frontend/track';
import {trackRegistry} from '../../frontend/track_registry';

import {TRACK_KIND} from './common';

/**
 * Demo track as so we can at least have two kinds of tracks.
 */
class CpuCounterTrack extends Track {
  static readonly kind = TRACK_KIND;
  static create(trackState: TrackState): CpuCounterTrack {
    return new CpuCounterTrack(trackState);
  }

  // No-op
  consumeData() {}

  constructor(trackState: TrackState) {
    super(trackState);
  }

  renderCanvas(
      ctx: CanvasRenderingContext2D, timeScale: TimeScale,
      visibleWindowMs: {start: number, end: number}): void {
    // It is possible to get width of track from visibleWindowMs.
    const visibleStartPx = timeScale.msToPx(visibleWindowMs.start);
    const visibleEndPx = timeScale.msToPx(visibleWindowMs.end);
    const visibleWidthPx = visibleEndPx - visibleStartPx;

    ctx.fillStyle = '#eee';
    ctx.fillRect(
        Math.round(0.25 * visibleWidthPx),
        0,
        Math.round(0.5 * visibleWidthPx),
        this.trackState.height);
    ctx.font = '16px Arial';
    ctx.fillStyle = '#000';
    ctx.fillText(
        'Drawing ' + CpuCounterTrack.kind,
        Math.round(0.4 * visibleWidthPx),
        20);
  }
}

trackRegistry.register(CpuCounterTrack);
