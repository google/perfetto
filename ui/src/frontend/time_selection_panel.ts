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

import {timeToString} from '../common/time';
import {TimeSpan} from '../common/time';

import {globals} from './globals';
import {gridlines} from './gridline_helper';
import {Panel, PanelSize} from './panel';
import {TRACK_SHELL_WIDTH} from './track_constants';

export interface BBox {
  x: number;
  y: number;
  width: number;
  height: number;
}

// Draws a vertical line with two horizontal tails at the left and right and
// a label in the middle. It looks a bit like a stretched H:
// |--- Label ---|
// The |target| bounding box determines where to draw the H.
// The |bounds| bounding box gives the visible region, this is used to adjust
// the positioning of the label to ensure it is on screen.
function drawHBar(
    ctx: CanvasRenderingContext2D, target: BBox, bounds: BBox, label: string) {
  ctx.fillStyle = '#222';

  const xLeft = Math.floor(target.x);
  const xRight = Math.ceil(target.x + target.width);
  const yMid = Math.floor(target.height / 2 + target.y);
  const xWidth = xRight - xLeft;

  // Draw horizontal bar of the H.
  ctx.fillRect(xLeft, yMid, xWidth, 1);
  // Draw left vertical bar of the H.
  ctx.fillRect(xLeft, target.y, 1, target.height);
  // Draw right vertical bar of the H.
  ctx.fillRect(xRight, target.y, 1, target.height);

  const labelWidth = ctx.measureText(label).width;

  // Find a good position for the label:
  // By default put the label in the middle of the H:
  let labelXLeft = Math.floor(xWidth / 2 - labelWidth / 2 + xLeft);

  if (labelWidth > target.width || labelXLeft < bounds.x ||
      (labelXLeft + labelWidth) > (bounds.x + bounds.width)) {
    // It won't fit in the middle or would be at least partly out of bounds
    // so put it either to the left or right:
    if (xRight > bounds.x + bounds.width) {
      // If the H extends off the right side of the screen the label
      // goes on the left of the H.
      labelXLeft = xLeft - labelWidth - 3;
    } else {
      // Otherwise the label goes on the right of the H.
      labelXLeft = xRight + 3;
    }
  }

  ctx.fillStyle = '#ffffff';
  ctx.fillRect(labelXLeft - 1, 0, labelWidth + 1, target.height);

  ctx.textBaseline = 'middle';
  ctx.fillStyle = '#222';
  ctx.font = '10px Google Sans';
  ctx.fillText(label, labelXLeft, yMid);
}

export class TimeSelectionPanel extends Panel {
  view() {
    return m('.time-selection-panel');
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    const range = globals.frontendLocalState.visibleWindowTime;
    const timeScale = globals.frontendLocalState.timeScale;

    ctx.fillStyle = '#999';
    ctx.fillRect(TRACK_SHELL_WIDTH - 1, 0, 2, size.height);
    for (const xAndTime of gridlines(size.width, range, timeScale)) {
      ctx.fillRect(xAndTime[0], 0, 1, size.height);
    }

    const selection = globals.state.currentSelection;
    if (selection !== null && selection.kind === `TIMESPAN`) {
      const start = Math.min(selection.startTs, selection.endTs);
      const end = Math.max(selection.startTs, selection.endTs);
      this.renderSpan(ctx, size, new TimeSpan(start, end));
    }
  }

  renderSpan(ctx: CanvasRenderingContext2D, size: PanelSize, span: TimeSpan) {
    const timeScale = globals.frontendLocalState.timeScale;
    const xLeft = timeScale.timeToPx(span.start);
    const xRight = timeScale.timeToPx(span.end);
    const label = timeToString(span.duration);
    drawHBar(
        ctx,
        {
          x: TRACK_SHELL_WIDTH + xLeft,
          y: 0,
          width: xRight - xLeft,
          height: size.height
        },
        {x: 0, y: 0, width: size.width, height: size.height},
        label);
  }
}
