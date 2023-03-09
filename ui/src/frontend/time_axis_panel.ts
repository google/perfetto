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

import {TRACK_SHELL_WIDTH} from './css_constants';
import {globals} from './globals';
import {
  TickGenerator,
  TickType,
  timeScaleForVisibleWindow,
} from './gridline_helper';
import {Panel, PanelSize} from './panel';

export class TimeAxisPanel extends Panel {
  view() {
    return m('.time-axis-panel');
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    ctx.fillStyle = '#999';
    ctx.font = '10px Roboto Condensed';
    ctx.textAlign = 'left';

    const startTime = timeToString(globals.state.traceTime.startSec);
    ctx.fillText(startTime + ' +', 6, 11);

    // Draw time axis.
    const timeScale = timeScaleForVisibleWindow(TRACK_SHELL_WIDTH, size.width);
    if (timeScale.timeSpan.duration > 0 && timeScale.widthPx > 0) {
      const tickGen = new TickGenerator(timeScale);
      for (const {type, time, position} of tickGen) {
        if (type === TickType.MAJOR) {
          ctx.fillRect(position, 0, 1, size.height);
          ctx.fillText(time.toFixed(tickGen.digits) + ' s', position + 5, 10);
        }
      }
    }

    ctx.fillRect(TRACK_SHELL_WIDTH - 2, 0, 2, size.height);
  }
}
