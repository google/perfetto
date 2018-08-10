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
import {globals} from '../../frontend/globals';
import {Track} from '../../frontend/track';
import {trackRegistry} from '../../frontend/track_registry';
import {
  ChromeSlice,
  ChromeSliceTrackData,
  TRACK_KIND,
  TRACK_PADDING
} from './common';

const SLICE_HEIGHT = 30;

// TODO: Pick a better color pallette.
const COLORS = [
  '#470000',
  '#773d00',
  '#795600',
  '#486b00',
  '#40199a',
  '#005c73',
  '#003d44',
  '#1e4f18',
];

function sliceIsVisible(
    slice: {start: number, end: number},
    visibleWindowMs: {start: number, end: number}) {
  return slice.end > visibleWindowMs.start && slice.start < visibleWindowMs.end;
}

class ChromeSliceTrack extends Track {
  static readonly kind = TRACK_KIND;
  static create(trackState: TrackState): ChromeSliceTrack {
    return new ChromeSliceTrack(trackState);
  }

  private trackData: ChromeSliceTrackData|undefined;
  private hoveredSlice: ChromeSlice|null = null;
  // TODO: Should this be in the controller?
  private titleToColorMap: Map<string, string>;
  private lastPickedColor = 0;

  constructor(trackState: TrackState) {
    super(trackState);
    this.titleToColorMap = new Map();
  }

  consumeData(trackData: ChromeSliceTrackData) {
    this.trackData = trackData;
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    if (!this.trackData) return;
    const {timeScale, visibleWindowMs} = globals.frontendLocalState;
    ctx.font = '12px monospace';
    // measuretext is expensive so we only use it once.
    const charWidth = ctx.measureText('a').width;

    for (const slice of this.trackData.slices) {
      if (!sliceIsVisible(slice, visibleWindowMs)) continue;
      const rectXStart = timeScale.msToPx(slice.start) as number;
      const rectXEnd = timeScale.msToPx(slice.end) as number;
      const rectWidth = rectXEnd - rectXStart;
      const rectYStart = TRACK_PADDING + slice.depth * SLICE_HEIGHT;

      if (slice === this.hoveredSlice) {
        ctx.fillStyle = '#b35846';
      } else {
        let color = this.titleToColorMap.get(slice.title);
        if (color === undefined) {
          this.lastPickedColor = (this.lastPickedColor + 1) % COLORS.length;
          color = COLORS[this.lastPickedColor];
          this.titleToColorMap.set(slice.title, color);
        }
        ctx.fillStyle = color;
      }

      ctx.fillRect(rectXStart, rectYStart, rectWidth, SLICE_HEIGHT);

      // Measuretext is expensive. Assume each character is 10px for now.
      const nameLength = slice.title.length * charWidth;
      ctx.fillStyle = 'white';
      const maxTextWidth = rectWidth - 10;
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
      ctx.fillText(displayText, rectXStart + 5, rectYStart + SLICE_HEIGHT / 2);
    }
  }

  onMouseMove({x, y}: {x: number, y: number}) {
    if (!this.trackData) return;
    const {timeScale} = globals.frontendLocalState;
    if (y < 40 || y > 70) {
      this.hoveredSlice = null;
      return;
    }
    const xMs = timeScale.pxToMs(x);
    this.hoveredSlice = null;

    for (const slice of this.trackData.slices) {
      if (slice.start <= xMs && slice.end >= xMs) {
        this.hoveredSlice = slice;
      }
    }
  }

  onMouseOut() {
    this.hoveredSlice = null;
  }
}

trackRegistry.register(ChromeSliceTrack);
