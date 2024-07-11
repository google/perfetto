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

import {Time} from '../base/time';

import {TRACK_SHELL_WIDTH} from './css_constants';
import {globals} from './globals';
import {getMaxMajorTicks, generateTicks, TickType} from './gridline_helper';
import {Size} from '../base/geom';
import {Panel} from './panel_container';
import {PxSpan, TimeScale} from './time_scale';
import {canvasClip} from '../common/canvas_utils';

// This is used to display the summary of search results.
export class TickmarkPanel implements Panel {
  readonly kind = 'panel';
  readonly selectable = false;

  render(): m.Children {
    return m('.tickbar');
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: Size) {
    const trackSize = {...size, width: size.width - TRACK_SHELL_WIDTH};

    ctx.fillStyle = '#999';
    ctx.fillRect(TRACK_SHELL_WIDTH - 2, 0, 2, size.height);

    ctx.save();
    ctx.translate(TRACK_SHELL_WIDTH, 0);
    canvasClip(ctx, 0, 0, trackSize.width, trackSize.height);
    this.renderPanel(ctx, trackSize);
    ctx.restore();
  }

  private renderPanel(ctx: CanvasRenderingContext2D, size: Size): void {
    const visibleWindow = globals.timeline.visibleWindow;
    const timescale = new TimeScale(visibleWindow, new PxSpan(0, size.width));
    const timespan = visibleWindow.toTimeSpan();

    if (size.width > 0 && timespan.duration > 0n) {
      const maxMajorTicks = getMaxMajorTicks(size.width);

      const offset = globals.timestampOffset();
      const tickGen = generateTicks(timespan, maxMajorTicks, offset);
      for (const {type, time} of tickGen) {
        const px = Math.floor(timescale.timeToPx(time));
        if (type === TickType.MAJOR) {
          ctx.fillRect(px, 0, 1, size.height);
        }
      }
    }

    const data = globals.searchSummary;
    for (let i = 0; i < data.tsStarts.length; i++) {
      const tStart = Time.fromRaw(data.tsStarts[i]);
      const tEnd = Time.fromRaw(data.tsEnds[i]);
      if (!visibleWindow.overlaps(tStart, tEnd)) {
        continue;
      }
      const rectStart = Math.max(timescale.timeToPx(tStart), 0);
      const rectEnd = timescale.timeToPx(tEnd);
      ctx.fillStyle = '#ffe263';
      ctx.fillRect(
        Math.floor(rectStart),
        0,
        Math.ceil(rectEnd - rectStart),
        size.height,
      );
    }
    const index = globals.state.searchIndex;
    if (index !== -1 && index < globals.currentSearchResults.tses.length) {
      const start = globals.currentSearchResults.tses[index];
      if (start !== -1n) {
        const triangleStart = Math.max(
          timescale.timeToPx(Time.fromRaw(start)),
          0,
        );
        ctx.fillStyle = '#000';
        ctx.beginPath();
        ctx.moveTo(triangleStart, size.height);
        ctx.lineTo(triangleStart - 3, 0);
        ctx.lineTo(triangleStart + 3, 0);
        ctx.lineTo(triangleStart, size.height);
        ctx.fill();
        ctx.closePath();
      }
    }

    ctx.restore();
  }
}
