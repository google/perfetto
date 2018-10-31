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

import {assertTrue} from '../../base/logging';
import {Actions} from '../../common/actions';
import {TrackState} from '../../common/state';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {Track} from '../../frontend/track';
import {trackRegistry} from '../../frontend/track_registry';

import {
  Config,
  COUNTER_TRACK_KIND,
  Data,
} from './common';

// 0.5 Makes the horizontal lines sharp.
const MARGIN_TOP = 5.5;
const RECT_HEIGHT = 30;

function getCurResolution() {
  // Truncate the resolution to the closest power of 10.
  const resolution = globals.frontendLocalState.timeScale.deltaPxToDuration(1);
  return Math.pow(10, Math.floor(Math.log10(resolution)));
}

function computeHue(name: string, ref: number) {
  let hue = 128;
  for (let i = 0; i < name.length; i++) {
    hue += name.charCodeAt(i);
  }
  hue += ref;
  return hue % 256;
}

class CounterTrack extends Track<Config, Data> {
  static readonly kind = COUNTER_TRACK_KIND;
  static create(trackState: TrackState): CounterTrack {
    return new CounterTrack(trackState);
  }

  private reqPending = false;
  private mouseXpos = 0;
  private hoveredValue: number|undefined = undefined;
  private hue: number;

  constructor(trackState: TrackState) {
    super(trackState);
    this.hue = computeHue(this.config.name, this.config.ref);
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

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
        ctx,
        timeScale.timeToPx(visibleWindowTime.start),
        timeScale.timeToPx(visibleWindowTime.end),
        timeScale.timeToPx(data.start),
        timeScale.timeToPx(data.end));

    assertTrue(data.timestamps.length === data.values.length);

    const startPx = Math.floor(timeScale.timeToPx(visibleWindowTime.start));
    const bottomY = MARGIN_TOP + RECT_HEIGHT;

    let lastX = startPx;
    let lastY = bottomY;

    ctx.fillStyle = `hsl(${this.hue}, 50%, 60%)`;
    ctx.beginPath();
    ctx.moveTo(lastX, lastY);
    for (let i = 0; i < data.values.length; i++) {
      const value = data.values[i];
      const startTime = data.timestamps[i];

      lastX = Math.floor(timeScale.timeToPx(startTime));

      const height = Math.round(RECT_HEIGHT * (1 - value / data.maximumValue));
      ctx.lineTo(lastX, lastY);
      lastY = MARGIN_TOP + height;
      ctx.lineTo(lastX, lastY);
    }
    ctx.lineTo(lastX, bottomY);
    ctx.closePath();
    ctx.fill();

    if (this.hoveredValue) {
      // TODO(hjd): Add units.
      const text = `value: ${this.hoveredValue.toLocaleString()}`;

      ctx.font = '10px Google Sans';
      const width = ctx.measureText(text).width;

      ctx.fillStyle = 'rgba(255, 255, 255, 0.9)';
      ctx.fillRect(this.mouseXpos, MARGIN_TOP, width + 16, RECT_HEIGHT);
      ctx.fillStyle = 'hsl(200, 50%, 40%)';
      ctx.textAlign = 'left';
      ctx.fillText(text, this.mouseXpos + 8, 18);
    }
  }

  onMouseMove({x}: {x: number, y: number}) {
    const data = this.data();
    if (data === undefined) return;
    this.mouseXpos = x;
    const {timeScale} = globals.frontendLocalState;
    const time = timeScale.pxToTime(x);
    this.hoveredValue = undefined;

    for (let i = 0; i < data.values.length; i++) {
      if (data.timestamps[i] > time) break;
      this.hoveredValue = data.values[i];
    }
  }

  onMouseOut() {
    this.hoveredValue = undefined;
  }
}

trackRegistry.register(CounterTrack);
