// Copyright (C) 2020 The Android Open Source Project
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
import {hueForSlice} from '../../common/colorizer';
import {TrackState} from '../../common/state';
import {fromNs, toNs} from '../../common/time';
import {globals} from '../../frontend/globals';
import {TimeScale} from '../../frontend/time_scale';
import {Track} from '../../frontend/track';
import {trackRegistry} from '../../frontend/track_registry';

import {Config, CPU_PROFILE_TRACK_KIND, Data} from './common';

const MARGIN_TOP = 4.5;
const RECT_HEIGHT = 30.5;

function colorForSample(callsiteId: number, isHovered: boolean): string {
  const hue = hueForSlice(String(callsiteId));
  const lightness = isHovered ? '55%' : '70%';
  return `hsl(${hue}, 45%, ${lightness})`;
}

class CpuProfileTrack extends Track<Config, Data> {
  static readonly kind = CPU_PROFILE_TRACK_KIND;
  static create(trackState: TrackState): CpuProfileTrack {
    return new CpuProfileTrack(trackState);
  }

  private centerY = this.getHeight() / 2;
  private markerWidth = (this.getHeight() - MARGIN_TOP) / 2;
  private hoveredTs: number|undefined = undefined;

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
      const selection = globals.state.currentSelection;
      const isHovered = this.hoveredTs === centerX;
      const isSelected = selection !== null &&
          selection.kind === 'CPU_PROFILE_SAMPLE' && selection.ts === centerX;
      const strokeWidth = isSelected ? 3 : 0;
      this.drawMarker(
          ctx,
          timeScale.timeToPx(fromNs(centerX)),
          this.centerY,
          isHovered,
          strokeWidth,
          data.callsiteId[i]);
    }
  }

  drawMarker(
      ctx: CanvasRenderingContext2D, x: number, y: number, isHovered: boolean,
      strokeWidth: number, callsiteId: number): void {
    ctx.beginPath();
    ctx.moveTo(x - this.markerWidth, y - this.markerWidth);
    ctx.lineTo(x, y + this.markerWidth);
    ctx.lineTo(x + this.markerWidth, y - this.markerWidth);
    ctx.lineTo(x - this.markerWidth, y - this.markerWidth);
    ctx.closePath();
    ctx.fillStyle = colorForSample(callsiteId, isHovered);
    ctx.fill();
    if (strokeWidth > 0) {
      ctx.strokeStyle = colorForSample(callsiteId, false);
      ctx.lineWidth = strokeWidth;
      ctx.stroke();
    }
  }

  onMouseMove({x, y}: {x: number, y: number}) {
    const data = this.data();
    if (data === undefined) return;
    const {timeScale} = globals.frontendLocalState;
    const time = toNs(timeScale.pxToTime(x));
    const [left, right] = searchSegment(data.tsStarts, time);
    const index = this.findTimestampIndex(left, timeScale, data, x, y, right);
    this.hoveredTs = index === -1 ? undefined : data.tsStarts[index];
  }

  onMouseOut() {
    this.hoveredTs = undefined;
  }

  onMouseClick({x, y}: {x: number, y: number}) {
    const data = this.data();
    if (data === undefined) return false;
    const {timeScale} = globals.frontendLocalState;

    const time = toNs(timeScale.pxToTime(x));
    const [left, right] = searchSegment(data.tsStarts, time);

    const index = this.findTimestampIndex(left, timeScale, data, x, y, right);

    if (index !== -1) {
      const id = data.ids[index];
      const ts = data.tsStarts[index];

      globals.makeSelection(
          Actions.selectCpuProfileSample({id, utid: this.config.utid, ts}));
      return true;
    }
    return false;
  }

  // If the markers overlap the rightmost one will be selected.
  findTimestampIndex(
      left: number, timeScale: TimeScale, data: Data, x: number, y: number,
      right: number): number {
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
    return index;
  }

  isInMarker(x: number, y: number, centerX: number) {
    return Math.abs(x - centerX) + Math.abs(y - this.centerY) <=
        this.markerWidth;
  }
}

trackRegistry.register(CpuProfileTrack);
