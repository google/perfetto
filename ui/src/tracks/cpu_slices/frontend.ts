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
import {drawGridLines} from '../../frontend/gridline_helper';
import {Milliseconds, TimeScale} from '../../frontend/time_scale';
import {Track} from '../../frontend/track';
import {trackRegistry} from '../../frontend/track_registry';
import {VirtualCanvasContext} from '../../frontend/virtual_canvas_context';
import {TRACK_KIND} from './common';

class CpuSliceTrack extends Track {
  static readonly kind = TRACK_KIND;
  static create(trackState: TrackState): CpuSliceTrack {
    return new CpuSliceTrack(trackState);
  }

  constructor(trackState: TrackState) {
    super(trackState);
  }

  renderCanvas(
      vCtx: VirtualCanvasContext, width: number, timeScale: TimeScale,
      visibleWindowMs: {start: number, end: number}): void {
    const sliceStart: Milliseconds = 1100000;
    const sliceEnd: Milliseconds = 1400000;

    const rectStart = timeScale.msToPx(sliceStart);
    const rectWidth = timeScale.msToPx(sliceEnd) - rectStart;

    let shownStart = rectStart as number;
    let shownWidth = rectWidth;

    if (shownStart < 0) {
      shownWidth += shownStart;
      shownStart = 0;
    }
    if (shownStart > width) {
      shownStart = width;
      shownWidth = 0;
    }
    if (shownStart + shownWidth > width) {
      shownWidth = width - shownStart;
    }

    vCtx.fillStyle = '#ccc';
    vCtx.fillRect(0, 0, width, 73);

    drawGridLines(
        vCtx,
        timeScale,
        [visibleWindowMs.start, visibleWindowMs.end],
        width,
        73);

    vCtx.fillStyle = '#c00';
    vCtx.fillRect(shownStart, 40, shownWidth, 30);

    vCtx.font = '16px Arial';
    vCtx.fillStyle = '#000';
    vCtx.fillText(this.trackState.kind + ' rendered by canvas', shownStart, 60);
  }
}

trackRegistry.register(CpuSliceTrack);