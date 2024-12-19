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
import {canvasClip} from '../../base/canvas_utils';
import {Size2D} from '../../base/geom';
import {assertUnreachable} from '../../base/logging';
import {Time, time, toISODateOnly} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {timestampFormat} from '../../core/timestamp_format';
import {TimestampFormat} from '../../public/timeline';
import {Trace} from '../../public/trace';
import {TRACK_SHELL_WIDTH} from '../css_constants';
import {
  generateTicks,
  getMaxMajorTicks,
  MIN_PX_PER_STEP,
  TickType,
} from './gridline_helper';

export class TimeAxisPanel {
  readonly id = 'time-axis-panel';
  readonly height = 22;

  constructor(private readonly trace: Trace) {}

  render(): m.Children {
    return m('', {style: {height: `${this.height}px`}});
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: Size2D) {
    ctx.fillStyle = '#999';
    ctx.textAlign = 'left';
    ctx.font = '11px Roboto Condensed';

    this.renderOffsetTimestamp(ctx);

    const trackSize = {...size, width: size.width - TRACK_SHELL_WIDTH};
    ctx.save();
    ctx.translate(TRACK_SHELL_WIDTH, 0);
    canvasClip(ctx, 0, 0, trackSize.width, trackSize.height);
    this.renderPanel(ctx, trackSize);
    ctx.restore();

    ctx.fillRect(TRACK_SHELL_WIDTH - 1, 0, 1, size.height);
  }

  private renderOffsetTimestamp(ctx: CanvasRenderingContext2D): void {
    const offset = this.trace.timeline.timestampOffset();
    const timestampFormat = this.trace.timeline.timestampFormat;
    switch (timestampFormat) {
      case TimestampFormat.TraceNs:
      case TimestampFormat.TraceNsLocale:
        break;
      case TimestampFormat.Seconds:
      case TimestampFormat.Milliseconds:
      case TimestampFormat.Microseconds:
      case TimestampFormat.Timecode:
        const width = renderTimestamp(ctx, offset, 6, 10, MIN_PX_PER_STEP);
        ctx.fillText('+', 6 + width + 2, 10, 6);
        break;
      case TimestampFormat.UTC:
        const offsetDate = Time.toDate(
          this.trace.traceInfo.utcOffset,
          this.trace.traceInfo.realtimeOffset,
        );
        const dateStr = toISODateOnly(offsetDate);
        ctx.fillText(`UTC ${dateStr}`, 6, 10);
        break;
      case TimestampFormat.TraceTz:
        const offsetTzDate = Time.toDate(
          this.trace.traceInfo.traceTzOffset,
          this.trace.traceInfo.realtimeOffset,
        );
        const dateTzStr = toISODateOnly(offsetTzDate);
        ctx.fillText(dateTzStr, 6, 10);
        break;
      default:
        assertUnreachable(timestampFormat);
    }
  }

  private renderPanel(ctx: CanvasRenderingContext2D, size: Size2D): void {
    const visibleWindow = this.trace.timeline.visibleWindow;
    const timescale = new TimeScale(visibleWindow, {
      left: 0,
      right: size.width,
    });
    const timespan = visibleWindow.toTimeSpan();
    const offset = this.trace.timeline.timestampOffset();

    // Draw time axis.
    if (size.width > 0 && timespan.duration > 0n) {
      const maxMajorTicks = getMaxMajorTicks(size.width);
      const tickGen = generateTicks(timespan, maxMajorTicks, offset);
      for (const {type, time} of tickGen) {
        if (type === TickType.MAJOR) {
          const position = Math.floor(timescale.timeToPx(time));
          ctx.fillRect(position, 0, 1, size.height);
          const domainTime = this.trace.timeline.toDomainTime(time);
          renderTimestamp(ctx, domainTime, position + 5, 10, MIN_PX_PER_STEP);
        }
      }
    }
  }
}

function renderTimestamp(
  ctx: CanvasRenderingContext2D,
  time: time,
  x: number,
  y: number,
  minWidth: number,
) {
  const fmt = timestampFormat();
  switch (fmt) {
    case TimestampFormat.UTC:
    case TimestampFormat.TraceTz:
    case TimestampFormat.Timecode:
      return renderTimecode(ctx, time, x, y, minWidth);
    case TimestampFormat.TraceNs:
      return renderRawTimestamp(ctx, time.toString(), x, y, minWidth);
    case TimestampFormat.TraceNsLocale:
      return renderRawTimestamp(ctx, time.toLocaleString(), x, y, minWidth);
    case TimestampFormat.Seconds:
      return renderRawTimestamp(ctx, Time.formatSeconds(time), x, y, minWidth);
    case TimestampFormat.Milliseconds:
      return renderRawTimestamp(
        ctx,
        Time.formatMilliseconds(time),
        x,
        y,
        minWidth,
      );
    case TimestampFormat.Microseconds:
      return renderRawTimestamp(
        ctx,
        Time.formatMicroseconds(time),
        x,
        y,
        minWidth,
      );
    default:
      const z: never = fmt;
      throw new Error(`Invalid timestamp ${z}`);
  }
}

// Print a time on the canvas in raw format.
function renderRawTimestamp(
  ctx: CanvasRenderingContext2D,
  time: string,
  x: number,
  y: number,
  minWidth: number,
) {
  ctx.font = '11px Roboto Condensed';
  ctx.fillText(time, x, y, minWidth);
  return ctx.measureText(time).width;
}

// Print a timecode over 2 lines with this formatting:
// DdHH:MM:SS
// mmm uuu nnn
// Returns the resultant width of the timecode.
function renderTimecode(
  ctx: CanvasRenderingContext2D,
  time: time,
  x: number,
  y: number,
  minWidth: number,
): number {
  const timecode = Time.toTimecode(time);
  ctx.font = '11px Roboto Condensed';

  const {dhhmmss} = timecode;
  const thinSpace = '\u2009';
  const subsec = timecode.subsec(thinSpace);
  ctx.fillText(dhhmmss, x, y, minWidth);
  const {width: firstRowWidth} = ctx.measureText(subsec);

  ctx.font = '10.5px Roboto Condensed';
  ctx.fillText(subsec, x, y + 10, minWidth);
  const {width: secondRowWidth} = ctx.measureText(subsec);

  return Math.max(firstRowWidth, secondRowWidth);
}
