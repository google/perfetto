// Copyright (C) 2019 The Android Open Source Project
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
import {fromNs} from '../../common/time';
import {globals} from '../../frontend/globals';
import {Track} from '../../frontend/track';
import {trackRegistry} from '../../frontend/track_registry';
import {Config, Data, HEAP_PROFILE_TRACK_KIND} from './common';

// 0.5 Makes the horizontal lines sharp.
const MARGIN_TOP = 4.5;
const RECT_HEIGHT = 30.5;

class HeapProfileTrack extends Track<Config, Data> {
  static readonly kind = HEAP_PROFILE_TRACK_KIND;
  static create(trackState: TrackState): HeapProfileTrack {
    return new HeapProfileTrack(trackState);
  }

  constructor(trackState: TrackState) {
    super(trackState);
  }

  getHeight() {
    return MARGIN_TOP + RECT_HEIGHT - 1;
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    const {
      timeScale,
    } = globals.frontendLocalState;
    const data = this.data();

    if (data === undefined) return;

    for (let i = 0; i < data.tsStarts.length; i++) {
      const ts = data.tsStarts[i];
      this.drawMarker(ctx, timeScale.timeToPx(fromNs(ts)), this.getHeight());
    }
  }

  drawMarker(ctx: CanvasRenderingContext2D, x: number, y: number): void {
    ctx.fillStyle = '#d9b3ff';
    ctx.beginPath();
    ctx.moveTo(x, MARGIN_TOP / 2);
    ctx.lineTo(x - 15, y / 2);
    ctx.lineTo(x, y - MARGIN_TOP / 2);
    ctx.lineTo(x + 15, y / 2);
    ctx.lineTo(x, MARGIN_TOP / 2);
    ctx.fill();
    ctx.closePath();
  }
}

trackRegistry.register(HeapProfileTrack);
