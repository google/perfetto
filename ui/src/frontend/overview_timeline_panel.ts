// Copyright (C) 2018 The Android Open Source Project
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

import * as m from 'mithril';

import {assertExists} from '../base/logging';
import {TimeSpan, timeToString} from '../common/time';

import {DragGestureHandler} from './drag_gesture_handler';
import {globals} from './globals';
import {Panel, PanelSize} from './panel';
import {TimeScale} from './time_scale';

export class OverviewTimelinePanel extends Panel {
  private width = 0;
  private dragStartPx = 0;
  private gesture?: DragGestureHandler;
  private timeScale?: TimeScale;
  private totTime = new TimeSpan(0, 0);

  // Must explicitly type now; arguments types are no longer auto-inferred.
  // https://github.com/Microsoft/TypeScript/issues/1373
  onupdate({dom}: m.CVnodeDOM) {
    this.width = dom.getBoundingClientRect().width;
    this.totTime = new TimeSpan(
        globals.state.traceTime.startSec, globals.state.traceTime.endSec);
    this.timeScale = new TimeScale(this.totTime, [0, assertExists(this.width)]);

    if (this.gesture === undefined) {
      this.gesture = new DragGestureHandler(
          dom as HTMLElement,
          this.onDrag.bind(this),
          this.onDragStart.bind(this),
          this.onDragEnd.bind(this));
    }
  }

  oncreate(vnode: m.CVnodeDOM) {
    this.onupdate(vnode);
  }

  view() {
    return m('.overview-timeline');
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    if (this.width === undefined) return;
    if (this.timeScale === undefined) return;
    const headerHeight = 25;
    const tracksHeight = size.height - headerHeight;

    // Draw time labels on the top header.
    ctx.font = '10px Google Sans';
    ctx.fillStyle = '#999';
    for (let i = 0; i < 100; i++) {
      const xPos = i * this.width / 100;
      const t = this.timeScale.pxToTime(xPos);
      if (xPos < 0) continue;
      if (xPos > this.width) break;
      if (i % 10 === 0) {
        ctx.fillRect(xPos, 0, 1, headerHeight - 5);
        ctx.fillText(timeToString(t - this.totTime.start), xPos + 5, 18);
      } else {
        ctx.fillRect(xPos, 0, 1, 5);
      }
    }

    // Draw mini-tracks with quanitzed density for each process.
    if (globals.overviewStore.size > 0) {
      const numTracks = globals.overviewStore.size;
      let hue = 128;
      let y = 0;
      const trackHeight = (tracksHeight - 2) / numTracks;
      for (const key of globals.overviewStore.keys()) {
        const loads = globals.overviewStore.get(key)!;
        for (let i = 0; i < loads.length; i++) {
          const xStart = this.timeScale.timeToPx(loads[i].startSec);
          const xEnd = this.timeScale.timeToPx(loads[i].endSec);
          const yOff = headerHeight + y * trackHeight;
          const lightness = Math.ceil((1 - loads[i].load * 0.7) * 100);
          ctx.fillStyle = `hsl(${hue}, 50%, ${lightness}%)`;
          ctx.fillRect(xStart, yOff, xEnd - xStart, trackHeight);
        }
        y++;
        hue = (hue + 32) % 256;
      }
    }

    // Draw bottom border.
    ctx.fillStyle = 'hsl(219, 40%, 50%)';
    ctx.fillRect(0, size.height - 2, this.width, 2);

    // Draw semi-opaque rects that occlude the non-visible time range.
    const vizTime = globals.frontendLocalState.visibleWindowTime;
    const vizStartPx = this.timeScale.timeToPx(vizTime.start);
    const vizEndPx = this.timeScale.timeToPx(vizTime.end);

    ctx.fillStyle = 'rgba(200, 200, 200, 0.8)';
    ctx.fillRect(0, headerHeight, vizStartPx, tracksHeight);
    ctx.fillRect(vizEndPx, headerHeight, this.width - vizEndPx, tracksHeight);

    // Draw brushes.
    const handleWidth = 3;
    const handleHeight = 25;
    const y = headerHeight + (tracksHeight - handleHeight) / 2;
    ctx.fillStyle = '#333';
    ctx.fillRect(vizStartPx, headerHeight, 1, tracksHeight);
    ctx.fillRect(vizEndPx, headerHeight, 1, tracksHeight);
    ctx.fillRect(vizStartPx - handleWidth, y, handleWidth, handleHeight);
    ctx.fillRect(vizEndPx + 1, y, handleWidth, handleHeight);
  }

  onDrag(x: number) {
    // Set visible time limits from selection.
    if (this.timeScale === undefined) return;
    let tStart = this.timeScale.pxToTime(this.dragStartPx);
    let tEnd = this.timeScale.pxToTime(x);
    if (tStart > tEnd) [tStart, tEnd] = [tEnd, tStart];
    const vizTime = new TimeSpan(tStart, tEnd);
    globals.frontendLocalState.updateVisibleTime(vizTime);
    globals.rafScheduler.scheduleRedraw();
  }

  onDragStart(x: number) {
    this.dragStartPx = x;
  }

  onDragEnd() {
    this.dragStartPx = 0;
  }
}
