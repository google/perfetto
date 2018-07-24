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
import {GridlineHelper} from '../../frontend/gridline_helper';
import {Milliseconds, TimeScale} from '../../frontend/time_scale';
import {TrackImpl} from '../../frontend/track_impl';
import {trackRegistry} from '../../frontend/track_registry';
import {VirtualCanvasContext} from '../../frontend/virtual_canvas_context';

class CpuSliceTrack extends TrackImpl {
  static readonly type = 'CpuSliceTrack';
  static create(trackState: TrackState): CpuSliceTrack {
    return new CpuSliceTrack(trackState);
  }

  constructor(trackState: TrackState) {
    super(trackState);
  }

  draw(vCtx: VirtualCanvasContext, width: number, timeScale: TimeScale): void {
    const sliceStart: Milliseconds = 100000;
    const sliceEnd: Milliseconds = 400000;

    const rectStart = timeScale.msToPx(sliceStart);
    const rectWidth = timeScale.msToPx(sliceEnd) - rectStart;
    const shownStart = rectStart > width ? width : rectStart;
    const shownWidth =
        rectWidth + (rectStart as number) > width ? width : rectWidth;

    vCtx.fillStyle = '#ccc';
    vCtx.fillRect(0, 0, width, 73);

    GridlineHelper.drawGridLines(vCtx, timeScale, [0, 1000000], width, 73);

    vCtx.fillStyle = '#c00';
    vCtx.fillRect(shownStart, 40, shownWidth, 30);

    vCtx.font = '16px Arial';
    vCtx.fillStyle = '#000';
    vCtx.fillText(this.trackState.name + ' rendered by canvas', shownStart, 60);
  }
}

trackRegistry.register(CpuSliceTrack);