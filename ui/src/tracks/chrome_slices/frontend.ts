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

import {requestTrackData} from '../../common/actions';
import {TrackState} from '../../common/state';
import {globals} from '../../frontend/globals';
import {Track} from '../../frontend/track';
import {trackRegistry} from '../../frontend/track_registry';

import {
  ChromeSliceTrackData,
  SLICE_TRACK_KIND,
} from './common';

const SLICE_HEIGHT = 30;
const TRACK_PADDING = 5;

function hash(s: string): number {
  let hash = 0x811c9dc5 & 0xfffffff;
  for (let i = 0; i < s.length; i++) {
    hash ^= s.charCodeAt(i);
    hash = (hash * 16777619) & 0xffffffff;
  }
  return hash & 0xff;
}

function getCurResolution() {
  // Truncate the resolution to the closest power of 10.
  const resolution = globals.frontendLocalState.timeScale.deltaPxToDuration(1);
  return Math.pow(10, Math.floor(Math.log10(resolution)));
}

class ChromeSliceTrack extends Track {
  static readonly kind = SLICE_TRACK_KIND;
  static create(trackState: TrackState): ChromeSliceTrack {
    return new ChromeSliceTrack(trackState);
  }

  private hoveredTitleId = -1;
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

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    ctx.font = '12px Google Sans';
    if (trackData.start > visibleWindowTime.start) {
      const rectWidth =
          timeScale.timeToPx(Math.min(trackData.start, visibleWindowTime.end));
      ctx.fillStyle = '#eee';
      ctx.fillRect(0, TRACK_PADDING, rectWidth, SLICE_HEIGHT);
      ctx.fillStyle = '#666';
      ctx.fillText(
          'loading...',
          rectWidth / 2,
          TRACK_PADDING + SLICE_HEIGHT / 2,
          rectWidth);
    }
    if (trackData.end < visibleWindowTime.end) {
      const rectX =
          timeScale.timeToPx(Math.max(trackData.end, visibleWindowTime.start));
      const rectWidth = timeScale.timeToPx(visibleWindowTime.end) - rectX;
      ctx.fillStyle = '#eee';
      ctx.fillRect(rectX, TRACK_PADDING, rectWidth, SLICE_HEIGHT);
      ctx.fillStyle = '#666';
      ctx.fillText(
          'loading...',
          rectX + rectWidth / 2,
          TRACK_PADDING + SLICE_HEIGHT / 2,
          rectWidth);
    }

    ctx.font = '12px Google Sans';
    ctx.textAlign = 'center';

    // measuretext is expensive so we only use it once.
    const charWidth = ctx.measureText('abcdefghij').width / 10;
    const pxEnd = timeScale.timeToPx(visibleWindowTime.end);

    for (let i = 0; i < trackData.starts.length; i++) {
      const tStart = trackData.starts[i];
      const tEnd = trackData.ends[i];
      const depth = trackData.depths[i];
      const cat = trackData.strings[trackData.categories[i]];
      const titleId = trackData.titles[i];
      const title = trackData.strings[titleId];
      if (tEnd <= visibleWindowTime.start || tStart >= visibleWindowTime.end) {
        continue;
      }
      const rectXStart = Math.max(timeScale.timeToPx(tStart), 0);
      const rectXEnd = Math.min(timeScale.timeToPx(tEnd), pxEnd);
      const rectWidth = rectXEnd - rectXStart;
      if (rectWidth < 0.1) continue;
      const rectYStart = TRACK_PADDING + depth * SLICE_HEIGHT;

      const hovered = titleId === this.hoveredTitleId;
      const hue = hash(cat);
      const saturation = Math.min(20 + depth * 10, 70);
      ctx.fillStyle = `hsl(${hue}, ${saturation}%, ${hovered ? 30 : 65}%)`;

      ctx.fillRect(rectXStart, rectYStart, rectWidth, SLICE_HEIGHT);

      const nameLength = title.length * charWidth;
      ctx.fillStyle = 'white';
      const maxTextWidth = rectWidth - 15;
      let displayText = '';
      if (nameLength < maxTextWidth) {
        displayText = title;
      } else {
        // -3 for the 3 ellipsis.
        const displayedChars = Math.floor(maxTextWidth / charWidth) - 3;
        if (displayedChars > 3) {
          displayText = title.substring(0, displayedChars) + '...';
        }
      }
      const rectXCenter = rectXStart + rectWidth / 2;
      ctx.fillText(displayText, rectXCenter, rectYStart + SLICE_HEIGHT / 2);
    }
  }

  onMouseMove({x, y}: {x: number, y: number}) {
    const trackData = this.trackData;
    this.hoveredTitleId = -1;
    if (trackData === undefined) return;
    const {timeScale} = globals.frontendLocalState;
    if (y < TRACK_PADDING) return;
    const t = timeScale.pxToTime(x);
    const depth = Math.floor(y / SLICE_HEIGHT);
    for (let i = 0; i < trackData.starts.length; i++) {
      const tStart = trackData.starts[i];
      const tEnd = trackData.ends[i];
      const titleId = trackData.titles[i];
      if (tStart <= t && t <= tEnd && depth === trackData.depths[i]) {
        this.hoveredTitleId = titleId;
        break;
      }
    }
  }

  onMouseOut() {
    this.hoveredTitleId = -1;
  }

  getHeight() {
    return SLICE_HEIGHT * (this.trackState.maxDepth + 1) + 2 * TRACK_PADDING;
  }

  private get trackData(): ChromeSliceTrackData {
    return globals.trackDataStore.get(this.trackState.id) as
        ChromeSliceTrackData;
  }

  consumeData() {}
}

trackRegistry.register(ChromeSliceTrack);
