// Copyright (C) 2019 The Android Open Source Project
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

import {drawDoubleHeadedArrow} from '../common/canvas_utils';
import {translateState} from '../common/thread_state';
import {timeToCode} from '../common/time';

import {globals} from './globals';
import {Panel, PanelSize} from './panel';

interface SliceDetailsPanelAttrs {
  utid: number;
}

export class SliceDetailsPanel extends Panel<SliceDetailsPanelAttrs> {
  view({attrs}: m.CVnode<SliceDetailsPanelAttrs>) {
    const threadInfo = globals.threads.get(attrs.utid);
    const sliceInfo = globals.sliceDetails;
    if (threadInfo && sliceInfo.ts && sliceInfo.dur) {
      return m(
          '.details-panel',
          m('.details-panel-heading', `Slice Details:`),
          m('.details-table', [m('table', [
              m('tr', m('th', `PID`), m('td', `${threadInfo.pid}`)),
              m('tr',
                m('th', `Process name`),
                m('td', `${threadInfo.procName}`)),
              m('tr', m('th', `TID`), m('td', `${threadInfo.tid}`)),
              m('tr',
                m('th', `Thread name`),
                m('td', `${threadInfo.threadName}`)),
              m('tr',
                m('th', `Start time`),
                m('td', `${timeToCode(sliceInfo.ts)}`)),
              m('tr',
                m('th', `Duration`),
                m('td', `${timeToCode(sliceInfo.dur)}`)),
              m('tr', m('th', `Prio`), m('td', `${sliceInfo.priority}`)),
              m('tr',
                m('th', `End State`),
                m('td', `${translateState(sliceInfo.endState)}`))
            ])], ));
    }
  else {
      return m(
          '.details-panel', m('.details-panel-heading', `Slice Details:`, ));
  }
}
renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
  const details = globals.sliceDetails;
  // Show expanded details on the scheduling of the currently selected slice.
  if (details.wakeupTs && details.wakerUtid !== undefined) {
    const threadInfo = globals.threads.get(details.wakerUtid);
    // Draw separation line.
    ctx.fillStyle = '#3c4b5d';
    ctx.fillRect(size.width / 2, 10, 1, size.height - 10);
    ctx.font = '16px Google Sans';
    ctx.fillText('Scheduling Latency:', size.width / 2 + 30, 30);
    // Draw diamond and vertical line.
    const startDraw = {x: size.width / 2 + 30, y: 52};
    ctx.beginPath();
    ctx.moveTo(startDraw.x, startDraw.y + 28);
    ctx.fillStyle = 'black';
    ctx.lineTo(startDraw.x + 6, startDraw.y + 20);
    ctx.lineTo(startDraw.x, startDraw.y + 12);
    ctx.lineTo(startDraw.x - 6, startDraw.y + 20);
    ctx.fill();
    ctx.closePath();
    ctx.fillRect(startDraw.x - 1, startDraw.y, 2, 100);

    // Wakeup explanation text.
    ctx.font = '13px Google Sans';
    ctx.fillStyle = '#3c4b5d';
    if (threadInfo) {
      const displayText =
          `Wakeup @ ${
                      timeToCode(
                          details.wakeupTs - globals.state.traceTime.startSec)
                    } on CPU ${details.wakerCpu} by`;
      const processText = `P: ${threadInfo.procName} [${threadInfo.pid}]`;
      const threadText = `T: ${threadInfo.threadName} [${threadInfo.tid}]`;
      ctx.fillText(displayText, startDraw.x + 20, startDraw.y + 20);
      ctx.fillText(processText, startDraw.x + 20, startDraw.y + 37);
      ctx.fillText(threadText, startDraw.x + 20, startDraw.y + 55);
    }

    // Draw latency arrow and explanation text.
    drawDoubleHeadedArrow(ctx, startDraw.x, startDraw.y + 80, 60, true);
    if (details.ts) {
      const displayLatency =
          `Scheduling latency: ${
                                 timeToCode(
                                     details.ts -
                                     (details.wakeupTs -
                                      globals.state.traceTime.startSec))
                               }`;
      ctx.fillText(displayLatency, startDraw.x + 70, startDraw.y + 86);
      const explain1 =
          'This is the interval from when the task became eligible to run';
      const explain2 =
          '(e.g. because of notifying a wait queue it was suspended on) to';
      const explain3 = 'when it started running.';
      ctx.font = '10px Google Sans';
      ctx.fillText(explain1, startDraw.x + 70, startDraw.y + 86 + 16);
      ctx.fillText(explain2, startDraw.x + 70, startDraw.y + 86 + 16 + 12);
      ctx.fillText(explain3, startDraw.x + 70, startDraw.y + 86 + 16 + 24);
    }
  }
}
}
