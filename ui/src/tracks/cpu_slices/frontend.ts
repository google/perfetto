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
import {requestTrackData} from '../../common/actions';
import {TrackState} from '../../common/state';
import {globals} from '../../frontend/globals';
import {Track} from '../../frontend/track';
import {trackRegistry} from '../../frontend/track_registry';

import {CPU_SLICE_TRACK_KIND, CpuSliceTrackData} from './common';

const MARGIN_TOP = 5;
const RECT_HEIGHT = 30;

function cropText(str: string, charWidth: number, rectWidth: number) {
  const maxTextWidth = rectWidth - 4;
  let displayText = '';
  const nameLength = str.length * charWidth;
  if (nameLength < maxTextWidth) {
    displayText = str;
  } else {
    // -3 for the 3 ellipsis.
    const displayedChars = Math.floor(maxTextWidth / charWidth) - 3;
    if (displayedChars > 3) {
      displayText = str.substring(0, displayedChars) + '...';
    }
  }
  return displayText;
}

function getCurResolution() {
  // Truncate the resolution to the closest power of 10.
  const resolution = globals.frontendLocalState.timeScale.deltaPxToDuration(1);
  return Math.pow(10, Math.floor(Math.log10(resolution)));
}

class CpuSliceTrack extends Track {
  static readonly kind = CPU_SLICE_TRACK_KIND;
  static create(trackState: TrackState): CpuSliceTrack {
    return new CpuSliceTrack(trackState);
  }

  private hoveredUtid = -1;
  private mouseXpos?: number;
  private reqPending = false;

  constructor(trackState: TrackState) {
    super(trackState);
  }

  reqDataDeferred() {
    const {visibleWindowTime} = globals.frontendLocalState;
    const reqStart = visibleWindowTime.start - visibleWindowTime.duration;
    const reqEnd = visibleWindowTime.end + visibleWindowTime.duration;
    const reqRes = getCurResolution();
    this.reqPending = false;
    globals.dispatch(
        requestTrackData(this.trackState.id, reqStart, reqEnd, reqRes));
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    // TODO: fonts and colors should come from the CSS and not hardcoded here.

    const {timeScale, visibleWindowTime} = globals.frontendLocalState;
    const trackData = this.trackData;

    // If there aren't enough cached slices data in |trackData| request more to
    // the controller.
    const inRange = trackData !== undefined &&
        (visibleWindowTime.start >= trackData.start &&
         visibleWindowTime.end <= trackData.end);
    if (!inRange || trackData.resolution > getCurResolution()) {
      if (!this.reqPending) {
        this.reqPending = true;
        setTimeout(() => this.reqDataDeferred(), 50);
      }
      if (trackData === undefined) return;  // Can't possibly draw anything.
    }
    ctx.textAlign = 'center';
    ctx.font = '12px Google Sans';
    const charWidth = ctx.measureText('dbpqaouk').width / 8;

    // TODO: this needs to be kept in sync with the hue generation algorithm
    // of overview_timeline_panel.ts
    const hue = (128 + (32 * this.trackState.cpu)) % 256;

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    ctx.font = '12px Google Sans';
    if (trackData.start > visibleWindowTime.start) {
      const rectWidth =
          timeScale.timeToPx(Math.min(trackData.start, visibleWindowTime.end));
      ctx.fillStyle = '#eee';
      ctx.fillRect(0, MARGIN_TOP, rectWidth, RECT_HEIGHT);
      ctx.fillStyle = '#666';
      ctx.fillText(
          'loading...', rectWidth / 2, MARGIN_TOP + RECT_HEIGHT / 2, rectWidth);
    }
    if (trackData.end < visibleWindowTime.end) {
      const rectX =
          timeScale.timeToPx(Math.max(trackData.end, visibleWindowTime.start));
      const rectWidth = timeScale.timeToPx(visibleWindowTime.end) - rectX;
      ctx.fillStyle = '#eee';
      ctx.fillRect(rectX, MARGIN_TOP, rectWidth, RECT_HEIGHT);
      ctx.fillStyle = '#666';
      ctx.fillText(
          'loading...',
          rectX + rectWidth / 2,
          MARGIN_TOP + RECT_HEIGHT / 2,
          rectWidth);
    }

    assertTrue(trackData.starts.length === trackData.ends.length);
    assertTrue(trackData.starts.length === trackData.utids.length);
    for (let i = 0; i < trackData.starts.length; i++) {
      const tStart = trackData.starts[i];
      const tEnd = trackData.ends[i];
      const utid = trackData.utids[i];
      if (tEnd <= visibleWindowTime.start || tStart >= visibleWindowTime.end) {
        continue;
      }
      const rectStart = timeScale.timeToPx(tStart);
      const rectEnd = timeScale.timeToPx(tEnd);
      const rectWidth = rectEnd - rectStart;
      if (rectWidth < 0.1) continue;

      const hovered = this.hoveredUtid === utid;
      ctx.fillStyle = `hsl(${hue}, 50%, ${hovered ? 25 : 60}%`;
      ctx.fillRect(rectStart, MARGIN_TOP, rectEnd - rectStart, RECT_HEIGHT);

      // TODO: consider de-duplicating this code with the copied one from
      // chrome_slices/frontend.ts.
      let title = `[utid:${utid}]`;
      let subTitle = '';
      const threadInfo = globals.threads.get(utid);
      if (threadInfo !== undefined) {
        title = `${threadInfo.procName} [${threadInfo.pid}]`;
        subTitle = `${threadInfo.threadName} [${threadInfo.tid}]`;
      }
      title = cropText(title, charWidth, rectWidth);
      subTitle = cropText(subTitle, charWidth, rectWidth);
      const rectXCenter = rectStart + rectWidth / 2;
      ctx.fillStyle = '#fff';
      ctx.font = '12px Google Sans';
      ctx.fillText(title, rectXCenter, MARGIN_TOP + RECT_HEIGHT / 2 - 3);
      ctx.fillStyle = 'rgba(255, 255, 255, 0.6)';
      ctx.font = '10px Google Sans';
      ctx.fillText(subTitle, rectXCenter, MARGIN_TOP + RECT_HEIGHT / 2 + 11);
    }

    const hoveredThread = globals.threads.get(this.hoveredUtid);
    if (hoveredThread !== undefined) {
      const procTitle = `P: ${hoveredThread.procName} [${hoveredThread.pid}]`;
      const threadTitle =
          `T: ${hoveredThread.threadName} [${hoveredThread.tid}]`;

      ctx.font = '10px Google Sans';
      const procTitleWidth = ctx.measureText(procTitle).width;
      const threadTitleWidth = ctx.measureText(threadTitle).width;
      const width = Math.max(procTitleWidth, threadTitleWidth);

      ctx.fillStyle = 'rgba(255, 255, 255, 0.9)';
      ctx.fillRect(this.mouseXpos!, MARGIN_TOP, width + 16, RECT_HEIGHT);
      ctx.fillStyle = 'hsl(200, 50%, 40%)';
      ctx.textAlign = 'left';
      ctx.fillText(procTitle, this.mouseXpos! + 8, 18);
      ctx.fillText(threadTitle, this.mouseXpos! + 8, 28);
    }
  }

  onMouseMove({x, y}: {x: number, y: number}) {
    const trackData = this.trackData;
    this.mouseXpos = x;
    if (trackData === undefined) return;
    const {timeScale} = globals.frontendLocalState;
    if (y < MARGIN_TOP || y > MARGIN_TOP + RECT_HEIGHT) {
      this.hoveredUtid = -1;
      return;
    }
    const t = timeScale.pxToTime(x);
    this.hoveredUtid = -1;

    for (let i = 0; i < trackData.starts.length; i++) {
      const tStart = trackData.starts[i];
      const tEnd = trackData.ends[i];
      const utid = trackData.utids[i];
      if (tStart <= t && t <= tEnd) {
        this.hoveredUtid = utid;
        break;
      }
    }
  }

  onMouseOut() {
    this.hoveredUtid = -1;
    this.mouseXpos = 0;
  }

  private get trackData(): CpuSliceTrackData {
    return globals.trackDataStore.get(this.trackState.id) as CpuSliceTrackData;
  }
}

trackRegistry.register(CpuSliceTrack);
