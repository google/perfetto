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
import {CpuSliceTrackData, TRACK_KIND} from './common';

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

  constructor(trackState: TrackState) {
    super(trackState);
  }

  consumeData(trackData: CpuSliceTrackData) {
    this.trackData = trackData;
  }

  renderCanvas(
      ctx: CanvasRenderingContext2D, timeScale: TimeScale,
      visibleWindowMs: {start: number, end: number}): void {
    if (!this.trackData) return;
    for (const slice of this.trackData.slices) {
      if (!sliceIsVisible(slice, visibleWindowMs)) continue;
      const rectStart = timeScale.msToPx(slice.start);
      const rectEnd = timeScale.msToPx(slice.end);
      ctx.fillStyle = '#4682b4';
      ctx.fillRect(rectStart, 40, rectEnd - rectStart, 30);
    }
  }
}

trackRegistry.register(CpuSliceTrack);
