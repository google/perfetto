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
import {PerfettoMouseEvent} from './events';
import {search} from '../base/binary_search';
import {Actions} from '../common/actions';
import {selectCurrentSearchResult} from './search_handler';

// This is used to display the summary of search results.
export class TickmarkPanel extends Panel {
  protected indicators: SearchResultIndicator[] = [];

  view() {
    return m('.tickbar', {
      onclick: (e: PerfettoMouseEvent) => {
        const index = search(this.indicators.map((i) => i.x), e.offsetX);
        if (index === -1) {
          return;
        }
        const indicator = this.indicators[index];
        if (!indicator.isInRange(e.offsetX)) {
          return;
        }
        const clickTime = indicator.getClickTime(e.offsetX);
        const clickedIndex = search(globals.currentSearchResults.tsStarts,
          clickTime);
        if (clickedIndex === -1) {
          return;
        }
        e.stopPropagation();
        globals.dispatch(Actions.setSearchIndex({index: clickedIndex}));
        selectCurrentSearchResult();
      },
    });
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    this.indicators = [];
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
      ctx.fillStyle = '#dcdc3b';
      const x = Math.floor(rectStart);
      const w = Math.ceil(rectEnd - rectStart);
      ctx.fillRect(
          x,
          0,
          w,
          size.height);
      this.indicators.push(new SearchResultIndicator(x, w, segmentSpan));
    }
    const index = globals.state.searchIndex;
    if (index !== -1) {
      const start = globals.currentSearchResults.tsStarts[index];
      const triangleStart =
          Math.max(visibleTimeScale.tpTimeToPx(start), 0) + TRACK_SHELL_WIDTH;
      ctx.fillStyle = getCssStr('--main-foreground-color');
      ctx.strokeStyle = getCssStr('--main-background-color');
      ctx.beginPath();
      ctx.moveTo(triangleStart, size.height);
      ctx.lineTo(triangleStart - 3, 0);
      ctx.lineTo(triangleStart + 3, 0);
      ctx.lineTo(triangleStart, size.height);
      ctx.fill();
      ctx.stroke();
      ctx.closePath();
    }

    ctx.restore();
  }
}

class SearchResultIndicator {
  constructor(
    public x: number,
    public w: number,
    public segmentSpan: TPTimeSpan,
  ) {}

  public isInRange(clickX: number): boolean {
    return (clickX >= this.x) && (clickX <= (this.x + this.w));
  }

  public getClickTime(clickX: number): bigint {
    const duration = this.segmentSpan.end - this.segmentSpan.start;
    const durationUntilClick = duration *
      (BigInt(clickX) - BigInt(this.x)) / BigInt(this.w);
    return this.segmentSpan.start + durationUntilClick;
  }
}
