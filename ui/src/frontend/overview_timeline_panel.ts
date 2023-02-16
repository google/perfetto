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
import {hueForCpu} from '../common/colorizer';
import {TimeSpan} from '../common/time';

import {
  OVERVIEW_TIMELINE_NON_VISIBLE_COLOR,
  SIDEBAR_WIDTH,
  TRACK_SHELL_WIDTH,
} from './css_constants';
import {BorderDragStrategy} from './drag/border_drag_strategy';
import {DragStrategy} from './drag/drag_strategy';
import {InnerDragStrategy} from './drag/inner_drag_strategy';
import {OuterDragStrategy} from './drag/outer_drag_strategy';
import {DragGestureHandler} from './drag_gesture_handler';
import {globals} from './globals';
import {TickGenerator, TickType} from './gridline_helper';
import {Panel, PanelSize} from './panel';
import {TimeScale} from './time_scale';

export class OverviewTimelinePanel extends Panel {
  private static HANDLE_SIZE_PX = 7;

  private width = 0;
  private gesture?: DragGestureHandler;
  private timeScale?: TimeScale;
  private totTime = new TimeSpan(0, 0);
  private dragStrategy?: DragStrategy;
  private readonly boundOnMouseMove = this.onMouseMove.bind(this);

  // Must explicitly type now; arguments types are no longer auto-inferred.
  // https://github.com/Microsoft/TypeScript/issues/1373
  onupdate({dom}: m.CVnodeDOM) {
    this.width = dom.getBoundingClientRect().width;
    this.totTime = new TimeSpan(
        globals.state.traceTime.startSec, globals.state.traceTime.endSec);
    this.timeScale = new TimeScale(
        this.totTime, [TRACK_SHELL_WIDTH, assertExists(this.width)]);

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
    (vnode.dom as HTMLElement)
        .addEventListener('mousemove', this.boundOnMouseMove);
  }

  onremove({dom}: m.CVnodeDOM) {
    (dom as HTMLElement)
        .removeEventListener('mousemove', this.boundOnMouseMove);
  }

  view() {
    return m('.overview-timeline');
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    if (this.width === undefined) return;
    if (this.timeScale === undefined) return;
    const headerHeight = 25;
    const tracksHeight = size.height - headerHeight;
    const timeSpan = new TimeSpan(0, this.totTime.duration);

    const timeScale = new TimeScale(timeSpan, [TRACK_SHELL_WIDTH, this.width]);

    if (timeScale.timeSpan.duration > 0 && timeScale.widthPx > 0) {
      const tickGen = new TickGenerator(timeScale);

      // Draw time labels on the top header.
      ctx.font = '10px Roboto Condensed';
      ctx.fillStyle = '#999';
      for (const {type, time, position} of tickGen) {
        const xPos = Math.round(position);
        if (xPos <= 0) continue;
        if (xPos > this.width) break;
        if (type === TickType.MAJOR) {
          ctx.fillRect(xPos - 1, 0, 1, headerHeight - 5);
          ctx.fillText(time.toFixed(tickGen.digits) + ' s', xPos + 5, 18);
        } else if (type == TickType.MEDIUM) {
          ctx.fillRect(xPos - 1, 0, 1, 8);
        } else if (type == TickType.MINOR) {
          ctx.fillRect(xPos - 1, 0, 1, 5);
        }
      }
    }

    // Draw mini-tracks with quanitzed density for each process.
    if (globals.overviewStore.size > 0) {
      const numTracks = globals.overviewStore.size;
      let y = 0;
      const trackHeight = (tracksHeight - 1) / numTracks;
      for (const key of globals.overviewStore.keys()) {
        const loads = globals.overviewStore.get(key)!;
        for (let i = 0; i < loads.length; i++) {
          const xStart = Math.floor(this.timeScale.timeToPx(loads[i].startSec));
          const xEnd = Math.ceil(this.timeScale.timeToPx(loads[i].endSec));
          const yOff = Math.floor(headerHeight + y * trackHeight);
          const lightness = Math.ceil((1 - loads[i].load * 0.7) * 100);
          ctx.fillStyle = `hsl(${hueForCpu(y)}, 50%, ${lightness}%)`;
          ctx.fillRect(xStart, yOff, xEnd - xStart, Math.ceil(trackHeight));
        }
        y++;
      }
    }

    // Draw bottom border.
    ctx.fillStyle = '#dadada';
    ctx.fillRect(0, size.height - 1, this.width, 1);

    // Draw semi-opaque rects that occlude the non-visible time range.
    const [vizStartPx, vizEndPx] =
        OverviewTimelinePanel.extractBounds(this.timeScale);

    ctx.fillStyle = OVERVIEW_TIMELINE_NON_VISIBLE_COLOR;
    ctx.fillRect(
        TRACK_SHELL_WIDTH - 1,
        headerHeight,
        vizStartPx - TRACK_SHELL_WIDTH,
        tracksHeight);
    ctx.fillRect(vizEndPx, headerHeight, this.width - vizEndPx, tracksHeight);

    // Draw brushes.
    ctx.fillStyle = '#999';
    ctx.fillRect(vizStartPx - 1, headerHeight, 1, tracksHeight);
    ctx.fillRect(vizEndPx, headerHeight, 1, tracksHeight);

    const hbarWidth = OverviewTimelinePanel.HANDLE_SIZE_PX;
    const hbarDivisionFactor = 3.5;
    // Draw handlebar
    ctx.fillRect(
        vizStartPx - Math.floor(hbarWidth / 2) - 1,
        headerHeight,
        hbarWidth,
        tracksHeight / hbarDivisionFactor);
    ctx.fillRect(
        vizEndPx - Math.floor(hbarWidth / 2),
        headerHeight,
        hbarWidth,
        tracksHeight / hbarDivisionFactor);
  }

  private onMouseMove(e: MouseEvent) {
    if (this.gesture === undefined || this.gesture.isDragging) {
      return;
    }
    (e.target as HTMLElement).style.cursor = this.chooseCursor(e.x);
  }

  private chooseCursor(x: number) {
    if (this.timeScale === undefined) return 'default';
    const [vizStartPx, vizEndPx] =
        OverviewTimelinePanel.extractBounds(this.timeScale);
    const startBound = vizStartPx - 1 + SIDEBAR_WIDTH;
    const endBound = vizEndPx + SIDEBAR_WIDTH;
    if (OverviewTimelinePanel.inBorderRange(x, startBound) ||
        OverviewTimelinePanel.inBorderRange(x, endBound)) {
      return 'ew-resize';
    } else if (x < SIDEBAR_WIDTH + TRACK_SHELL_WIDTH) {
      return 'default';
    } else if (x < startBound || endBound < x) {
      return 'crosshair';
    } else {
      return 'all-scroll';
    }
  }

  onDrag(x: number) {
    if (this.dragStrategy === undefined) return;
    this.dragStrategy.onDrag(x);
  }

  onDragStart(x: number) {
    if (this.timeScale === undefined) return;
    const pixelBounds = OverviewTimelinePanel.extractBounds(this.timeScale);
    if (OverviewTimelinePanel.inBorderRange(x, pixelBounds[0]) ||
        OverviewTimelinePanel.inBorderRange(x, pixelBounds[1])) {
      this.dragStrategy = new BorderDragStrategy(this.timeScale, pixelBounds);
    } else if (x < pixelBounds[0] || pixelBounds[1] < x) {
      this.dragStrategy = new OuterDragStrategy(this.timeScale);
    } else {
      this.dragStrategy = new InnerDragStrategy(this.timeScale, pixelBounds);
    }
    this.dragStrategy.onDragStart(x);
  }

  onDragEnd() {
    this.dragStrategy = undefined;
  }

  private static extractBounds(timeScale: TimeScale): [number, number] {
    const vizTime = globals.frontendLocalState.getVisibleStateBounds();
    return [
      Math.floor(timeScale.timeToPx(vizTime[0])),
      Math.ceil(timeScale.timeToPx(vizTime[1])),
    ];
  }

  private static inBorderRange(a: number, b: number): boolean {
    return Math.abs(a - b) < this.HANDLE_SIZE_PX / 2;
  }
}
