// Copyright (C) 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import * as m from 'mithril';

import {timeToString} from '../common/time';

import {globals} from './globals';
import {DESIRED_PX_PER_STEP, getGridStepSize} from './gridline_helper';
import {Panel, PanelSize} from './panel';
import {TRACK_SHELL_WIDTH} from './track_panel';

export class TimeAxisPanel extends Panel {
  view() {
    return m('.time-axis-panel');
  }


  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    const timeScale = globals.frontendLocalState.timeScale;
    ctx.font = '10px Google Sans';
    ctx.fillStyle = '#999';

    const range = globals.frontendLocalState.visibleWindowTime;
    const desiredSteps = size.width / DESIRED_PX_PER_STEP;
    const step = getGridStepSize(range.duration, desiredSteps);
    const start = Math.round(range.start / step) * step;

    for (let s = start; s < range.end; s += step) {
      let xPos = TRACK_SHELL_WIDTH;
      xPos += Math.floor(timeScale.timeToPx(s));
      if (xPos < 0) continue;
      if (xPos > size.width) break;
      ctx.fillRect(xPos, 0, 1, size.height);
      ctx.fillText(timeToString(s - range.start), xPos + 5, 10);
    }
  }
}
