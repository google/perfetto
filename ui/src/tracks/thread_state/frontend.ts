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


import {search, searchEq} from '../../base/binary_search';
import {Actions} from '../../common/actions';
import {TrackState} from '../../common/state';
import {translateState} from '../../common/thread_state';
import {cropText} from '../../common/track_utils';
import {colorForState} from '../../frontend/colorizer';
import {globals} from '../../frontend/globals';
import {Track} from '../../frontend/track';
import {trackRegistry} from '../../frontend/track_registry';

import {
  Config,
  Data,
  THREAD_STATE_TRACK_KIND,
} from './common';

const MARGIN_TOP = 5;
const RECT_HEIGHT = 12;

class ThreadStateTrack extends Track<Config, Data> {
  static readonly kind = THREAD_STATE_TRACK_KIND;
  static create(trackState: TrackState): ThreadStateTrack {
    return new ThreadStateTrack(trackState);
  }

  constructor(trackState: TrackState) {
    super(trackState);
  }

  getHeight(): number {
    return 22;
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    const {timeScale, visibleWindowTime} = globals.frontendLocalState;
    const data = this.data();
    const charWidth = ctx.measureText('dbpqaouk').width / 8;

    // If there aren't enough cached slices data in |data| request more to
    // the controller.
    const inRange = data !== undefined &&
        (visibleWindowTime.start >= data.start &&
         visibleWindowTime.end <= data.end);
    if (!inRange || data === undefined ||
        data.resolution !== globals.getCurResolution()) {
      globals.requestTrackData(this.trackState.id);
    }
    if (data === undefined) return;  // Can't possibly draw anything.

    for (let i = 0; i < data.starts.length; i++) {
      const tStart = data.starts[i];
      const tEnd = data.ends[i];
      const state = data.strings[data.state[i]];
      if (tEnd <= visibleWindowTime.start || tStart >= visibleWindowTime.end) {
        continue;
      }
      if (tStart && tEnd) {
        const rectStart = timeScale.timeToPx(tStart);
        const rectEnd = timeScale.timeToPx(tEnd);
        const color = colorForState(state);
        ctx.fillStyle = `hsl(${color.h},${color.s}%,${color.l}%)`;
        const rectWidth = rectEnd - rectStart;
        ctx.fillRect(rectStart, MARGIN_TOP, rectWidth, RECT_HEIGHT);

        // Don't render text when we have less than 5px to play with.
        if (rectWidth < 5) continue;
        ctx.textAlign = 'center';
        const title = cropText(translateState(state), charWidth, rectWidth);
        const rectXCenter = rectStart + rectWidth / 2;
        ctx.fillStyle = '#fff';
        ctx.font = '10px Google Sans';
        ctx.fillText(title, rectXCenter, MARGIN_TOP + RECT_HEIGHT / 2 + 3);
      }
    }

    const selection = globals.state.currentSelection;
    if (selection !== null && selection.kind === 'THREAD_STATE' &&
        selection.utid === this.config.utid) {
      const [startIndex, endIndex] = searchEq(data.starts, selection.ts);
      if (startIndex !== endIndex) {
        const tStart = data.starts[startIndex];
        const tEnd = data.ends[startIndex];
        const state = data.strings[data.state[startIndex]];
        const rectStart = timeScale.timeToPx(tStart);
        const rectEnd = timeScale.timeToPx(tEnd);
        const color = colorForState(state);
        ctx.strokeStyle = `hsl(${color.h},${color.s}%,${color.l * 0.7}%)`;
        ctx.beginPath();
        ctx.lineWidth = 3;
        ctx.strokeRect(
            rectStart, MARGIN_TOP - 1.5, rectEnd - rectStart, RECT_HEIGHT + 3);
        ctx.closePath();
      }
    }
  }

  onMouseClick({x}: {x: number}) {
    const data = this.data();
    if (data === undefined) return false;
    const {timeScale} = globals.frontendLocalState;
    const time = timeScale.pxToTime(x);
    const index = search(data.starts, time);
    const ts = index === -1 ? undefined : data.starts[index];
    const tsEnd = index === -1 ? undefined : data.ends[index];
    const state = index === -1 ? undefined : data.strings[data.state[index]];
    const utid = this.config.utid;
    if (ts && state && tsEnd) {
      globals.dispatch(
          Actions.selectThreadState({utid, ts, dur: tsEnd - ts, state}));
      return true;
    }
    return false;
  }
}

trackRegistry.register(ThreadStateTrack);
