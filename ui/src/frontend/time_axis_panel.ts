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

import m from 'mithril';

import {
  tpTimeToSeconds,
  tpTimeToString,
} from '../common/time';

import {getCssStr, TRACK_SHELL_WIDTH} from './css_constants';
import {globals} from './globals';
import {
  getMaxMajorTicks,
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
    ctx.fillStyle = getCssStr('--main-foreground-color');
    ctx.font = '10px Roboto Condensed';
    ctx.textAlign = 'left';

    const startTime = tpTimeToString(globals.state.traceTime.start);
    ctx.fillText(startTime + ' +', 6, 11);

    ctx.save();
    ctx.beginPath();
    ctx.rect(TRACK_SHELL_WIDTH, 0, size.width - TRACK_SHELL_WIDTH, size.height);
    ctx.clip();

    // Draw time axis.
    const span = globals.frontendLocalState.visibleWindow.timestampSpan;
    if (size.width > TRACK_SHELL_WIDTH && span.duration > 0n) {
      const maxMajorTicks = getMaxMajorTicks(size.width - TRACK_SHELL_WIDTH);
      const map = timeScaleForVisibleWindow(TRACK_SHELL_WIDTH, size.width);
      const tickGen =
          new TickGenerator(span, maxMajorTicks, globals.state.traceTime.start);
      for (const {type, time} of tickGen) {
        const position = Math.floor(map.tpTimeToPx(time));
        const sec = tpTimeToSeconds(time - globals.state.traceTime.start);
        if (type === TickType.MAJOR) {
          ctx.fillRect(position, 0, 1, size.height);
          ctx.fillText(sec.toFixed(tickGen.digits) + ' s', position + 5, 10);
        }
      }
    }

    ctx.restore();

    ctx.fillRect(TRACK_SHELL_WIDTH - 2, 0, 2, size.height);
  }
}
