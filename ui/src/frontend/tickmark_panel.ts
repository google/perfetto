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

import {SearchSummary} from '../common/search_data';
import {globals} from './globals';
import {gridlines} from './gridline_helper';

import {Panel, PanelSize} from './panel';
import {TRACK_SHELL_WIDTH} from './track_constants';

// This is used to display the summary of search results.
export class TickmarkPanel extends Panel {
  // TODO(taylori): Replace this with real data.
  private all: number[] = [...Array.from<number>({length: 100}).keys()];
  private data: SearchSummary = {
    tsStarts: new Float64Array(this.all.filter(i => i % 2 === 0)),
    tsEnds: new Float64Array(this.all.filter(i => i % 2 === 1)),
    count: new Uint8Array(
        Array.from({length: 50}, () => Math.floor(Math.random() * 10))),
  };

  view() {
    return m('.tickbar');
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    const {timeScale, visibleWindowTime} = globals.frontendLocalState;

    ctx.fillStyle = '#999';
    for (const xAndTime of gridlines(
             size.width, visibleWindowTime, timeScale)) {
      ctx.fillRect(xAndTime[0], 0, 1, size.height);
    }

    const data = this.data;
    const maxCount = Math.max(...data.count);
    const colorInterval = 40 / maxCount;
    for (let i = 0; i < data.tsStarts.length; i++) {
      const tStart = data.tsStarts[i];
      const tEnd = data.tsEnds[i];
      const count = data.count[i];
      if (tEnd <= visibleWindowTime.start || tStart >= visibleWindowTime.end) {
        continue;
      }
      const rectStart = timeScale.timeToPx(tStart) + TRACK_SHELL_WIDTH;
      const rectEnd = timeScale.timeToPx(tEnd) + TRACK_SHELL_WIDTH;
      ctx.fillStyle =
          `hsl(59, 100%, ${50 + (Math.round(colorInterval * count))}%)`;
      ctx.fillRect(rectStart, 0, rectEnd - rectStart, size.height);
    }
  }
}
