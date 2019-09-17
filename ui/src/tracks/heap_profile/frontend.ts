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

import {searchSegment} from '../../base/binary_search';
import {Actions} from '../../common/actions';
import {TrackState} from '../../common/state';
import {fromNs, toNs} from '../../common/time';
import {globals} from '../../frontend/globals';
import {Track} from '../../frontend/track';
import {trackRegistry} from '../../frontend/track_registry';

import {Config, Data, HEAP_PROFILE_TRACK_KIND} from './common';

// 0.5 Makes the horizontal lines sharp.
const MARGIN_TOP = 4.5;
const RECT_HEIGHT = 30.5;

class HeapProfileTrack extends Track<Config, Data> {
  private centerY = this.getHeight() / 2;
  private width = (this.getHeight() - MARGIN_TOP) / 2;

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
      const centerX = data.tsStarts[i];
      this.drawMarker(ctx, timeScale.timeToPx(fromNs(centerX)), this.centerY);
    }
  }

  drawMarker(ctx: CanvasRenderingContext2D, x: number, y: number): void {
    ctx.fillStyle = '#d9b3ff';
    ctx.beginPath();
    ctx.moveTo(x, y - this.width);
    ctx.lineTo(x - this.width, y);
    ctx.lineTo(x, y + this.width);
    ctx.lineTo(x + this.width, y);
    ctx.lineTo(x, y - this.width);
    ctx.fill();
    ctx.closePath();
  }

  // TODO(tneda): Add a border to show the currently selected marker and
  // a hover state.
  onMouseClick({x, y}: {x: number, y: number}) {
    const data = this.data();
    if (data === undefined) return false;
    const {timeScale} = globals.frontendLocalState;

    const time = toNs(timeScale.pxToTime(x));
    const [left, right] = searchSegment(data.tsStarts, time);

    let index = -1;
    if (left !== -1) {
      const centerX = timeScale.timeToPx(fromNs(data.tsStarts[left]));
      if (this.isInMarker(x, y, centerX)) {
        index = left;
      }
    }
    if (right !== -1) {
      const centerX = timeScale.timeToPx(fromNs(data.tsStarts[right]));
      if (this.isInMarker(x, y, centerX)) {
        index = right;
      }
    }

    // If the markers overlap the rightmost one will be selected.
    if (index !== -1) {
      globals.makeSelection(Actions.selectHeapDump(
          {id: index, upid: this.config.upid, ts: data.tsStarts[index]}));
      return true;
    }
    return false;
  }

  isInMarker(x: number, y: number, centerX: number) {
    return Math.abs(x - centerX) + Math.abs(y - this.centerY) <= this.width;
  }
}

trackRegistry.register(HeapProfileTrack);
