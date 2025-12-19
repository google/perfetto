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
import {canvasClip} from '../../base/canvas_utils';
import {Size2D} from '../../base/geom';
import {TimeScale} from '../../base/time_scale';
import {getOrCreate} from '../../base/utils';
import {TraceImpl} from '../../core/trace_impl';
import {COLOR_BORDER, TRACK_SHELL_WIDTH} from '../css_constants';
import {generateTicks, getMaxMajorTicks, TickType} from './gridline_helper';
import {SearchOverviewTrack} from './search_overview_track';

// We want to create the overview track only once per trace, but this
// class can be delete and re-instantiated when switching between pages via
// the sidebar. So we cache the overview track and bind it to the lifetime of
// the TraceImpl object.
const trackTraceMap = new WeakMap<TraceImpl, SearchOverviewTrack>();

// This is used to display the summary of search results.
export class TickmarkPanel {
  private searchOverviewTrack: SearchOverviewTrack;
  readonly height = 5;

  constructor(private readonly trace: TraceImpl) {
    this.searchOverviewTrack = getOrCreate(
      trackTraceMap,
      trace,
      () => new SearchOverviewTrack(trace),
    );
  }

  render(): m.Children {
    return m('', {style: {height: `${this.height}px`}});
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: Size2D): void {
    ctx.fillStyle = COLOR_BORDER;
    ctx.fillRect(TRACK_SHELL_WIDTH - 1, 0, 1, size.height);

    const trackSize = {...size, width: size.width - TRACK_SHELL_WIDTH};
    ctx.save();
    ctx.translate(TRACK_SHELL_WIDTH, 0);
    canvasClip(ctx, 0, 0, trackSize.width, trackSize.height);
    this.renderTrack(ctx, trackSize);
    ctx.restore();
  }

  private renderTrack(ctx: CanvasRenderingContext2D, size: Size2D): void {
    const visibleWindow = this.trace.timeline.visibleWindow;
    const timescale = new TimeScale(visibleWindow, {
      left: 0,
      right: size.width,
    });
    const timespan = visibleWindow.toTimeSpan();

    if (size.width > 0 && timespan.duration > 0n) {
      const maxMajorTicks = getMaxMajorTicks(size.width);

      const offset = this.trace.timeline.getTimeAxisOrigin();
      const tickGen = generateTicks(timespan, maxMajorTicks, offset);
      for (const {type, time} of tickGen) {
        const px = Math.floor(timescale.timeToPx(time));
        if (type === TickType.MAJOR) {
          ctx.fillRect(px, 0, 1, size.height);
        }
      }
    }

    this.searchOverviewTrack.render(ctx, size);
  }
}
