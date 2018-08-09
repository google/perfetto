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
import {globals} from '../../frontend/globals';
import {Track} from '../../frontend/track';
import {trackRegistry} from '../../frontend/track_registry';
import {CpuSlice, CpuSliceTrackData, TRACK_KIND} from './common';

function sliceIsVisible(
    slice: {start: number, end: number},
    visibleWindowMs: {start: number, end: number}) {
  return slice.end > visibleWindowMs.start && slice.start < visibleWindowMs.end;
}

class CpuSliceTrack extends Track {
  static readonly kind = TRACK_KIND;
  static create(trackState: TrackState): CpuSliceTrack {
    return new CpuSliceTrack(trackState);
  }

  private trackData: CpuSliceTrackData|undefined;
  private hoveredSlice: CpuSlice|null = null;

  constructor(trackState: TrackState) {
    super(trackState);
  }

  consumeData(trackData: CpuSliceTrackData) {
    this.trackData = trackData;
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    if (!this.trackData) return;
    const {timeScale, visibleWindowMs} = globals.frontendLocalState;
    for (const slice of this.trackData.slices) {
      if (!sliceIsVisible(slice, visibleWindowMs)) continue;
      const rectStart = timeScale.msToPx(slice.start);
      const rectEnd = timeScale.msToPx(slice.end);
      ctx.fillStyle = slice === this.hoveredSlice ? '#b35846' : '#4682b4';
      ctx.fillRect(rectStart, 40, rectEnd - rectStart, 30);
    }
  }

  onMouseMove({x, y}: {x: number, y: number}) {
    if (!this.trackData) return;
    const {timeScale} = globals.frontendLocalState;
    if (y < 40 || y > 70) {
      this.hoveredSlice = null;
      return;
    }
    const xMs = timeScale.pxToMs(x);
    this.hoveredSlice = null;

    for (const slice of this.trackData.slices) {
      if (slice.start <= xMs && slice.end >= xMs) {
        this.hoveredSlice = slice;
      }
    }
  }

  onMouseOut() {
    this.hoveredSlice = null;
  }
}

trackRegistry.register(CpuSliceTrack);
