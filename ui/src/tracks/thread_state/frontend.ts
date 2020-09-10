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

import {search} from '../../base/binary_search';
import {Actions} from '../../common/actions';
import {cropText} from '../../common/canvas_utils';
import {colorForState} from '../../common/colorizer';
import {TrackState} from '../../common/state';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {Track} from '../../frontend/track';
import {trackRegistry} from '../../frontend/track_registry';

import {
  Config,
  Data,
  THREAD_STATE_TRACK_KIND,
} from './common';

const MARGIN_TOP = 4;
const RECT_HEIGHT = 14;
const EXCESS_WIDTH = 10;

class ThreadStateTrack extends Track<Config, Data> {
  static readonly kind = THREAD_STATE_TRACK_KIND;
  static create(trackState: TrackState): ThreadStateTrack {
    return new ThreadStateTrack(trackState);
  }

  constructor(trackState: TrackState) {
    super(trackState);
  }

  getHeight(): number {
    return 2 * MARGIN_TOP + RECT_HEIGHT;
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    const {timeScale, visibleWindowTime} = globals.frontendLocalState;
    const data = this.data();
    const charWidth = ctx.measureText('dbpqaouk').width / 8;

    if (data === undefined) return;  // Can't possibly draw anything.

    // The draw of the rect on the selected slice must happen after the other
    // drawings, otherwise it would result under another rect.
    let drawRectOnSelected = () => {};

    checkerboardExcept(
        ctx,
        this.getHeight(),
        timeScale.timeToPx(visibleWindowTime.start),
        timeScale.timeToPx(visibleWindowTime.end),
        timeScale.timeToPx(data.start),
        timeScale.timeToPx(data.end),
    );

    ctx.textAlign = 'center';
    ctx.font = '10px Roboto Condensed';

    for (let i = 0; i < data.starts.length; i++) {
      const tStart = data.starts[i];
      const tEnd = data.ends[i];
      const state = data.strings[data.state[i]];
      if (tEnd <= visibleWindowTime.start || tStart >= visibleWindowTime.end) {
        continue;
      }

      // Don't display a slice for Task Dead.
      if (state === 'x') continue;
      const rectStart = timeScale.timeToPx(tStart);
      const rectEnd = timeScale.timeToPx(tEnd);

      const currentSelection = globals.state.currentSelection;
      const isSelected = currentSelection &&
          currentSelection.kind === 'THREAD_STATE' &&
          currentSelection.id === data.ids[i];

      const color = colorForState(state);

      let colorStr = `hsl(${color.h},${color.s}%,${color.l}%)`;
      if (color.a) {
        colorStr = `hsla(${color.h},${color.s}%,${color.l}%, ${color.a})`;
      }
      ctx.fillStyle = colorStr;

      const rectWidth = rectEnd - rectStart;
      ctx.fillRect(rectStart, MARGIN_TOP, rectWidth, RECT_HEIGHT);

      // Don't render text when we have less than 10px to play with.
      if (rectWidth < 10 || state === 'Sleeping') continue;
      const title = cropText(state, charWidth, rectWidth);
      const rectXCenter = rectStart + rectWidth / 2;
      ctx.fillStyle = color.l > 80 ? '#404040' : '#fff';
      ctx.fillText(title, rectXCenter, MARGIN_TOP + RECT_HEIGHT / 2 + 3);

      if (isSelected) {
        drawRectOnSelected = () => {
          const rectStart =
              Math.max(0 - EXCESS_WIDTH, timeScale.timeToPx(tStart));
          const rectEnd = Math.min(
              timeScale.timeToPx(visibleWindowTime.end) + EXCESS_WIDTH,
              timeScale.timeToPx(tEnd));
          const color = colorForState(state);
          ctx.strokeStyle = `hsl(${color.h},${color.s}%,${color.l * 0.7}%)`;
          ctx.beginPath();
          ctx.lineWidth = 3;
          ctx.strokeRect(
              rectStart,
              MARGIN_TOP - 1.5,
              rectEnd - rectStart,
              RECT_HEIGHT + 3);
          ctx.closePath();
        };
      }
    }
    drawRectOnSelected();
  }

  onMouseClick({x}: {x: number}) {
    const data = this.data();
    if (data === undefined) return false;
    const {timeScale} = globals.frontendLocalState;
    const time = timeScale.pxToTime(x);
    const index = search(data.starts, time);
    if (index === -1) return false;

    const id = data.ids[index];
    globals.makeSelection(
        Actions.selectThreadState({id, trackId: this.trackState.id}));
    return true;
  }
}

trackRegistry.register(ThreadStateTrack);
