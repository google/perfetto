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

import m from 'mithril';

import {TPTimeSpan} from '../common/time';

import {getCssStr, TRACK_SHELL_WIDTH} from './css_constants';
import {globals} from './globals';
import {
  getMaxMajorTicks,
  TickGenerator,
  TickType,
  timeScaleForVisibleWindow,
} from './gridline_helper';
import {Panel, PanelSize} from './panel';

// This is used to display the summary of search results.
export class TickmarkPanel extends Panel {
  view() {
    return m('.tickbar');
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    const {visibleTimeScale} = globals.frontendLocalState;

    ctx.fillStyle = getCssStr('--main-foreground-color');
    ctx.fillRect(TRACK_SHELL_WIDTH - 2, 0, 2, size.height);

    ctx.save();
    ctx.beginPath();
    ctx.rect(TRACK_SHELL_WIDTH, 0, size.width - TRACK_SHELL_WIDTH, size.height);
    ctx.clip();

    const visibleSpan = globals.frontendLocalState.visibleWindow.timestampSpan;
    if (size.width > TRACK_SHELL_WIDTH && visibleSpan.duration > 0n) {
      const maxMajorTicks = getMaxMajorTicks(size.width - TRACK_SHELL_WIDTH);
      const map = timeScaleForVisibleWindow(TRACK_SHELL_WIDTH, size.width);
      for (const {type, time} of new TickGenerator(
               visibleSpan, maxMajorTicks, globals.state.traceTime.start)) {
        const px = Math.floor(map.tpTimeToPx(time));
        if (type === TickType.MAJOR) {
          ctx.fillRect(px, 0, 1, size.height);
        }
      }
    }

    const data = globals.searchSummary;
    for (let i = 0; i < data.tsStarts.length; i++) {
      const tStart = data.tsStarts[i];
      const tEnd = data.tsEnds[i];
      const segmentSpan = new TPTimeSpan(tStart, tEnd);
      if (!visibleSpan.intersects(segmentSpan)) {
        continue;
      }
      const rectStart =
          Math.max(visibleTimeScale.tpTimeToPx(tStart), 0) + TRACK_SHELL_WIDTH;
      const rectEnd = visibleTimeScale.tpTimeToPx(tEnd) + TRACK_SHELL_WIDTH;
      ctx.fillStyle = '#ffe263';
      ctx.fillRect(
          Math.floor(rectStart),
          0,
          Math.ceil(rectEnd - rectStart),
          size.height);
    }
    const index = globals.state.searchIndex;
    if (index !== -1) {
      const start = globals.currentSearchResults.tsStarts[index];
      const triangleStart =
          Math.max(visibleTimeScale.tpTimeToPx(start), 0) + TRACK_SHELL_WIDTH;
      ctx.fillStyle = '#000';
      ctx.beginPath();
      ctx.moveTo(triangleStart, size.height);
      ctx.lineTo(triangleStart - 3, 0);
      ctx.lineTo(triangleStart + 3, 0);
      ctx.lineTo(triangleStart, size.height);
      ctx.fill();
      ctx.closePath();
    }

    ctx.restore();
  }
}
