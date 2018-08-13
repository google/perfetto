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
import {fromNs, TimeSpan} from '../../common/time';
import {globals} from '../../frontend/globals';
import {Track} from '../../frontend/track';
import {trackRegistry} from '../../frontend/track_registry';

import {
  ChromeSlice,
  ChromeSliceTrackData,
  TRACK_KIND,
} from './common';

const SLICE_HEIGHT = 30;
const TRACK_PADDING = 5;

function sliceIsVisible(
    slice: {start: number, end: number}, visibleWindowTime: TimeSpan) {
  return fromNs(slice.end) > visibleWindowTime.start &&
      fromNs(slice.start) < visibleWindowTime.end;
}

function hash(s: string): number {
  let hash = 0x811c9dc5 & 0xfffffff;
  for (let i = 0; i < s.length; i++) {
    hash ^= s.charCodeAt(i);
    hash = (hash * 16777619) & 0xffffffff;
  }
  return hash & 0xff;
}

class ChromeSliceTrack extends Track {
  static readonly kind = TRACK_KIND;
  static create(trackState: TrackState): ChromeSliceTrack {
    return new ChromeSliceTrack(trackState);
  }

  private trackData: ChromeSliceTrackData|undefined;
  private hoveredSlice: ChromeSlice|null = null;

  constructor(trackState: TrackState) {
    super(trackState);
  }

  consumeData(trackData: ChromeSliceTrackData) {
    this.trackData = trackData;
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    if (!this.trackData) return;
    const {timeScale, visibleWindowTime} = globals.frontendLocalState;
    ctx.font = '12px Google Sans';
    ctx.textAlign = 'center';

    // measuretext is expensive so we only use it once.
    const charWidth = ctx.measureText('abcdefghij').width / 10;
    const pxEnd = timeScale.timeToPx(visibleWindowTime.end);

    for (const slice of this.trackData.slices) {
      if (!sliceIsVisible(slice, visibleWindowTime)) continue;
      const rectXStart = Math.max(timeScale.timeToPx(fromNs(slice.start)), 0);
      const rectXEnd = Math.min(timeScale.timeToPx(fromNs(slice.end)), pxEnd);
      const rectWidth = rectXEnd - rectXStart;
      if (rectWidth < 0.1) continue;
      const rectYStart = TRACK_PADDING + slice.depth * SLICE_HEIGHT;

      if (slice === this.hoveredSlice) {
        ctx.fillStyle = '#b35846';
      } else {
        const hue = hash(slice.title);
        const saturation = Math.min(30 + slice.depth * 10, 100);
        ctx.fillStyle = `hsl(${hue}, ${saturation}%, 40%)`;
      }

      ctx.fillRect(rectXStart, rectYStart, rectWidth, SLICE_HEIGHT);

      const nameLength = slice.title.length * charWidth;
      ctx.fillStyle = 'white';
      const maxTextWidth = rectWidth - 15;
      let displayText = '';
      if (nameLength < maxTextWidth) {
        displayText = slice.title;
      } else {
        // -3 for the 3 ellipsis.
        const displayedChars = Math.floor(maxTextWidth / charWidth) - 3;
        if (displayedChars > 3) {
          displayText = slice.title.substring(0, displayedChars) + '...';
        }
      }
      const rectXCenter = rectXStart + rectWidth / 2;
      ctx.fillText(displayText, rectXCenter, rectYStart + SLICE_HEIGHT / 2);
    }
  }

  onMouseMove({x, y}: {x: number, y: number}) {
    if (!this.trackData) return;
    const {timeScale} = globals.frontendLocalState;
    if (y < TRACK_PADDING || y > (SLICE_HEIGHT - TRACK_PADDING)) {
      this.hoveredSlice = null;
      return;
    }
    const t = timeScale.pxToTime(x);
    const depth = Math.floor(y / SLICE_HEIGHT);
    this.hoveredSlice = null;

    for (const slice of this.trackData.slices) {
      if (fromNs(slice.start) <= t && fromNs(slice.end) >= t &&
          slice.depth === depth) {
        this.hoveredSlice = slice;
      }
    }
  }

  onMouseOut() {
    this.hoveredSlice = null;
  }

  getHeight() {
    return SLICE_HEIGHT * (this.trackState.maxDepth + 1) + 2 * TRACK_PADDING;
  }
}

trackRegistry.register(ChromeSliceTrack);
