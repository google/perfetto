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

import {Timecode, toDomainTime, TPTime} from '../common/time';

import {TRACK_SHELL_WIDTH} from './css_constants';
import {globals} from './globals';
import {
  getMaxMajorTicks,
  MIN_PX_PER_STEP,
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
    ctx.textAlign = 'left';
    ctx.font = '11px Roboto Condensed';

    const traceStartTime = globals.state.traceTime.start;
    const width = renderTimecode(ctx, traceStartTime, 6, 10);
    ctx.fillText('+', 6 + width, 15, 6);

    ctx.save();
    ctx.beginPath();
    ctx.rect(TRACK_SHELL_WIDTH, 0, size.width - TRACK_SHELL_WIDTH, size.height);
    ctx.clip();

    // Draw time axis.
    const span = globals.frontendLocalState.visibleWindow.timestampSpan;
    if (size.width > TRACK_SHELL_WIDTH && span.duration > 0n) {
      const maxMajorTicks = getMaxMajorTicks(size.width - TRACK_SHELL_WIDTH);
      const map = timeScaleForVisibleWindow(TRACK_SHELL_WIDTH, size.width);
      const tickGen = new TickGenerator(span, maxMajorTicks, traceStartTime);
      for (const {type, time} of tickGen) {
        if (type === TickType.MAJOR) {
          const position = Math.floor(map.tpTimeToPx(time));
          ctx.fillRect(position, 0, 1, size.height);
          const relTime = toDomainTime(time);
          renderTimecode(ctx, relTime, position + 5, 10);
        }
      }
    }
    ctx.restore();
    ctx.fillRect(TRACK_SHELL_WIDTH - 2, 0, 2, size.height);
  }
}

// Print a timecode over 2 lines with this formatting:
// DdHH:MM:SS
// mmm uuu nnn
// Returns the resultant width of the timecode.
function renderTimecode(
    ctx: CanvasRenderingContext2D, time: TPTime, x: number, y: number): number {
  const timecode = new Timecode(time);
  ctx.font = '11px Roboto Condensed';

  const {dhhmmss} = timecode;
  const thinSpace = '\u2009';
  const subsec = timecode.subsec(thinSpace);
  ctx.fillText(dhhmmss, x, y, MIN_PX_PER_STEP);
  const {width: firstRowWidth} = ctx.measureText(subsec);

  ctx.font = '10.5px Roboto Condensed';
  ctx.fillText(subsec, x, y + 10, MIN_PX_PER_STEP);
  const {width: secondRowWidth} = ctx.measureText(subsec);

  return Math.max(firstRowWidth, secondRowWidth);
}
