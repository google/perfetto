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
import {assertTrue} from '../../base/logging';
import {Actions} from '../../common/actions';
import {TrackState} from '../../common/state';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {Track} from '../../frontend/track';
import {trackRegistry} from '../../frontend/track_registry';

import {
  Config,
  CPU_FREQ_TRACK_KIND,
  Data,
} from './common';

// 0.5 Makes the horizontal lines sharp.
const MARGIN_TOP = 4.5;
const RECT_HEIGHT = 30;

function getCurResolution() {
  // Truncate the resolution to the closest power of 10.
  const resolution = globals.frontendLocalState.timeScale.deltaPxToDuration(1);
  return Math.pow(10, Math.floor(Math.log10(resolution)));
}

class CpuFreqTrack extends Track<Config, Data> {
  static readonly kind = CPU_FREQ_TRACK_KIND;
  static create(trackState: TrackState): CpuFreqTrack {
    return new CpuFreqTrack(trackState);
  }

  private reqPending = false;
  private mouseXpos = 0;
  private hoveredValue: number|undefined = undefined;
  private hoveredTs: number|undefined = undefined;
  private hoveredTsEnd: number|undefined = undefined;

  constructor(trackState: TrackState) {
    super(trackState);
  }

  reqDataDeferred() {
    const {visibleWindowTime} = globals.frontendLocalState;
    const reqStart = visibleWindowTime.start - visibleWindowTime.duration;
    const reqEnd = visibleWindowTime.end + visibleWindowTime.duration;
    const reqRes = getCurResolution();
    this.reqPending = false;
    globals.dispatch(Actions.reqTrackData({
      trackId: this.trackState.id,
      start: reqStart,
      end: reqEnd,
      resolution: reqRes
    }));
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    // TODO: fonts and colors should come from the CSS and not hardcoded here.
    const {timeScale, visibleWindowTime} = globals.frontendLocalState;
    const data = this.data();

    // If there aren't enough cached slices data in |data| request more to
    // the controller.
    const inRange = data !== undefined &&
        (visibleWindowTime.start >= data.start &&
         visibleWindowTime.end <= data.end);
    if (!inRange || data === undefined ||
        data.resolution !== getCurResolution()) {
      if (!this.reqPending) {
        this.reqPending = true;
        setTimeout(() => this.reqDataDeferred(), 50);
      }
    }
    if (data === undefined) return;  // Can't possibly draw anything.

    assertTrue(data.timestamps.length === data.valuesKHz.length);

    const startPx = Math.floor(timeScale.timeToPx(visibleWindowTime.start));
    const endPx = Math.floor(timeScale.timeToPx(visibleWindowTime.end));
    const zeroY = MARGIN_TOP + RECT_HEIGHT / (data.minimumValue < 0 ? 2 : 1);

    let lastX = startPx;
    let lastY = zeroY;

    // Quantize the Y axis to quarters of powers of tens (7.5K, 10K, 12.5K).
    const maxValue = Math.max(data.maximumValue, 0);

    let yMax = Math.max(Math.abs(data.minimumValue), maxValue);
    const kUnits = ['', 'K', 'M', 'G', 'T', 'E'];
    const exp = Math.ceil(Math.log10(Math.max(yMax, 1)));
    const pow10 = Math.pow(10, exp);
    yMax = Math.ceil(yMax / (pow10 / 4)) * (pow10 / 4);
    const yRange = data.minimumValue < 0 ? yMax * 2 : yMax;
    const unitGroup = Math.floor(exp / 3);
    const num = yMax / Math.pow(10, unitGroup * 3);
    // The values we have for cpufreq are in kHz so +1 to unitGroup.
    const yLabel = `${num} ${kUnits[unitGroup + 1]}Hz`;

    const hue = (128 + (32 * this.config.cpu)) % 256;

    ctx.fillStyle = `hsl(${hue}, 45%, 85%)`;
    ctx.strokeStyle = `hsl(${hue}, 45%, 55%)`;
    ctx.beginPath();
    ctx.moveTo(lastX, lastY);
    for (let i = 0; i < data.valuesKHz.length; i++) {
      const value = data.valuesKHz[i];
      const startTime = data.timestamps[i];
      const nextY = zeroY - Math.round((value / yRange) * RECT_HEIGHT);
      if (nextY === lastY) continue;

      lastX = Math.floor(timeScale.timeToPx(startTime));
      ctx.lineTo(lastX, lastY);
      ctx.lineTo(lastX, nextY);
      lastY = nextY;
    }
    ctx.lineTo(endPx, lastY);
    ctx.lineTo(endPx, zeroY);
    ctx.closePath();
    ctx.fill();
    ctx.stroke();

    ctx.font = '10px Google Sans';

    if (this.hoveredValue !== undefined && this.hoveredTs !== undefined) {
      const text = `value: ${this.hoveredValue.toLocaleString()}kHz`;
      const width = ctx.measureText(text).width;

      ctx.fillStyle = `hsl(${hue}, 45%, 75%)`;
      ctx.strokeStyle = `hsl(${hue}, 45%, 45%)`;

      const xStart = Math.floor(timeScale.timeToPx(this.hoveredTs));
      const xEnd = this.hoveredTsEnd === undefined ?
          endPx :
          Math.floor(timeScale.timeToPx(this.hoveredTsEnd));
      const y = zeroY - Math.round((this.hoveredValue / yRange) * RECT_HEIGHT);

      // Highlight line.
      ctx.beginPath();
      ctx.moveTo(xStart, y);
      ctx.lineTo(xEnd, y);
      ctx.lineWidth = 3;
      ctx.stroke();
      ctx.lineWidth = 1;

      // Draw change marker.
      ctx.beginPath();
      ctx.arc(xStart, y, 3 /*r*/, 0 /*start angle*/, 2 * Math.PI /*end angle*/);
      ctx.fill();
      ctx.stroke();

      // Draw the tooltip.
      ctx.fillStyle = 'rgba(255, 255, 255, 0.8)';
      ctx.fillRect(this.mouseXpos + 5, MARGIN_TOP, width + 16, RECT_HEIGHT);
      ctx.fillStyle = 'hsl(200, 50%, 40%)';
      ctx.textAlign = 'left';
      ctx.textBaseline = 'middle';
      ctx.fillText(text, this.mouseXpos + 8, MARGIN_TOP + RECT_HEIGHT/2);
    }

    // Write the Y scale on the top left corner.
    ctx.fillStyle = 'rgba(255, 255, 255, 0.6)';
    ctx.fillRect(0, 0, 40, 16);
    ctx.fillStyle = '#666';
    ctx.textAlign = 'left';
    ctx.fillText(`${yLabel}`, 5, 14);

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
        ctx,
        timeScale.timeToPx(visibleWindowTime.start),
        timeScale.timeToPx(visibleWindowTime.end),
        timeScale.timeToPx(data.start),
        timeScale.timeToPx(data.end));
  }

  onMouseMove({x}: {x: number, y: number}) {
    const data = this.data();
    if (data === undefined) return;
    this.mouseXpos = x;
    const {timeScale} = globals.frontendLocalState;
    const time = timeScale.pxToTime(x);

    const [left, right] = searchSegment(data.timestamps, time);
    this.hoveredTs = left === -1 ? undefined : data.timestamps[left];
    this.hoveredTsEnd = right === -1 ? undefined : data.timestamps[right];
    this.hoveredValue = left === -1 ? undefined : data.valuesKHz[left];
  }

  onMouseOut() {
    this.hoveredValue = undefined;
    this.hoveredTs = undefined;
  }
}

trackRegistry.register(CpuFreqTrack);
