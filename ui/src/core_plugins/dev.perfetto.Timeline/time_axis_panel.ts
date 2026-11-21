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
import type {Size2D} from '../../base/geom';
import {assertUnreachable} from '../../base/assert';
import {Time, type time, formatDate} from '../../base/time';
import {formatDuration} from '../../components/time_utils';
import {TimeScale} from '../../base/time_scale';
import {TimestampFormat} from '../../public/timeline';
import type {Trace} from '../../public/trace';
import {
  COLOR_ACCENT,
  COLOR_BORDER,
  COLOR_TEXT_MUTED,
  COLOR_TEXT_ON_ACCENT,
  FONT_COMPACT,
  TRACK_SHELL_WIDTH,
} from '../../frontend/css_constants';
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

  renderCanvas(ctx: CanvasRenderingContext2D, size: Size2D, hoverX?: number) {
    ctx.textAlign = 'left';
    ctx.font = `11px ${FONT_COMPACT}`;

    this.renderOffsetTimestamp(ctx);

    const trackSize = {...size, width: size.width - TRACK_SHELL_WIDTH};
    ctx.save();
    ctx.translate(TRACK_SHELL_WIDTH, 0);
    canvasClip(ctx, 0, 0, trackSize.width, trackSize.height);
    const panelHoverX =
      hoverX !== undefined ? hoverX - TRACK_SHELL_WIDTH : undefined;
    this.renderPanel(ctx, trackSize, panelHoverX);
    ctx.restore();
  }

  private renderOffsetTimestamp(ctx: CanvasRenderingContext2D): void {
    ctx.fillStyle = COLOR_TEXT_MUTED;
    const {start, end} = this.trace.traceInfo;
    const dur = formatDuration(this.trace, Time.diff(end, start));
    this.renderLabelAndValue(ctx, 'Start: ', this.formatStart(), 6, 10);
    this.renderLabelAndValue(ctx, 'Duration: ', dur, 6, 20);
  }

  private formatStart(): string {
    const {start, tzOffMin} = this.trace.traceInfo;
    const {timestampFormat: fmt, customTimezoneOffset} = this.trace.timeline;
    switch (fmt) {
      case TimestampFormat.Timecode:
        return Time.toTimecode(start).toString(' ');
      case TimestampFormat.UTC:
        return this.formatStartAsWallClock(0);
      case TimestampFormat.CustomTimezone:
        return this.formatStartAsWallClock(customTimezoneOffset);
      case TimestampFormat.TraceTz:
        return this.formatStartAsWallClock(tzOffMin);
      case TimestampFormat.Seconds:
        return Time.formatSeconds(start);
      case TimestampFormat.Milliseconds:
        return Time.formatMilliseconds(start);
      case TimestampFormat.Microseconds:
        return Time.formatMicroseconds(start);
      case TimestampFormat.TraceNs:
        return start.toString();
      case TimestampFormat.TraceNsLocale:
        return start.toLocaleString();
      default:
        assertUnreachable(fmt);
    }
  }

  private formatStartAsWallClock(tzOffsetMins: number): string {
    const {start, unixOffset} = this.trace.traceInfo;
    const date = Time.toDate(start, unixOffset);
    return formatDate(date, {printTimezone: false, tzOffsetMins});
  }

  private renderLabelAndValue(
    ctx: CanvasRenderingContext2D,
    label: string,
    value: string,
    x: number,
    y: number,
  ): void {
    ctx.font = `bold 11px ${FONT_COMPACT}`;
    const labelWidth = ctx.measureText(label).width;
    ctx.fillText(label, x, y);
    ctx.font = `11px ${FONT_COMPACT}`;
    ctx.fillText(value, x + labelWidth, y);
  }

  private renderPanel(
    ctx: CanvasRenderingContext2D,
    size: Size2D,
    hoverX?: number,
  ): void {
    const visibleWindow = this.trace.timeline.visibleWindow;
    const timescale = new TimeScale(visibleWindow, {
      left: 0,
      right: size.width,
    });
    const timespan = visibleWindow.toTimeSpan();
    const offset = this.trace.timeline.getTimeAxisOrigin();

    // Draw time axis.
    if (size.width > 0 && timespan.duration > 0n) {
      const maxMajorTicks = getMaxMajorTicks(size.width);
      const tickGen = generateTicks(timespan, maxMajorTicks, offset);
      for (const {type, time} of tickGen) {
        if (type === TickType.MAJOR) {
          const position = Math.floor(timescale.timeToPx(time));
          ctx.fillStyle = COLOR_BORDER;
          ctx.fillRect(position, 0, 1, size.height);
          const domainTime = this.trace.timeline.toDomainTime(time);
          ctx.fillStyle = COLOR_TEXT_MUTED;
          this.renderTimestamp(
            ctx,
            domainTime,
            position + 5,
            10,
            MIN_PX_PER_STEP,
          );
        }
      }

      if (hoverX !== undefined && hoverX >= 0 && hoverX < size.width) {
        this.renderHoverTimestamp(ctx, timescale, size, hoverX);
      }
    }
  }

  private renderHoverTimestamp(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    size: Size2D,
    hoverX: number,
  ): void {
    const xPos = Math.floor(hoverX);
    const hoverTime = timescale.pxToHpTime(hoverX).toTime();
    const domainTime = this.trace.timeline.toDomainTime(hoverTime);
    const fmt = this.trace.timeline.timestampFormat;

    let firstLine: string;
    let secondLine: string | undefined;
    switch (fmt) {
      case TimestampFormat.UTC:
      case TimestampFormat.CustomTimezone:
      case TimestampFormat.TraceTz:
      case TimestampFormat.Timecode: {
        const timecode = Time.toTimecode(domainTime);
        firstLine = timecode.dhhmmss;
        secondLine = timecode.subsec('\u2009');
        break;
      }
      case TimestampFormat.TraceNs:
      case TimestampFormat.TraceNsLocale:
      case TimestampFormat.Seconds:
      case TimestampFormat.Milliseconds:
      case TimestampFormat.Microseconds:
        firstLine = formatRawTimestamp(fmt, domainTime);
        break;
      default:
        assertUnreachable(fmt);
    }

    ctx.font = `11px ${FONT_COMPACT}`;
    const firstRowWidth = ctx.measureText(firstLine).width;
    let secondRowWidth = 0;
    if (secondLine !== undefined) {
      ctx.font = `10.5px ${FONT_COMPACT}`;
      secondRowWidth = ctx.measureText(secondLine).width;
    }
    const textWidth = Math.ceil(Math.max(firstRowWidth, secondRowWidth));

    const padding = 4;
    const bgWidth = padding + textWidth + padding;
    const placeOnLeft = xPos + bgWidth > size.width;
    const bgLeft = placeOnLeft ? xPos + 1 - bgWidth : xPos;
    const labelX = bgLeft + padding;

    ctx.fillStyle = COLOR_ACCENT;
    ctx.fillRect(bgLeft, 0, bgWidth, size.height);

    ctx.fillStyle = COLOR_TEXT_ON_ACCENT;
    ctx.font = `11px ${FONT_COMPACT}`;
    ctx.fillText(firstLine, labelX, 10);
    if (secondLine !== undefined) {
      ctx.font = `10.5px ${FONT_COMPACT}`;
      ctx.fillText(secondLine, labelX, 20);
    }
  }

  private renderTimestamp(
    ctx: CanvasRenderingContext2D,
    time: time,
    x: number,
    y: number,
    minWidth: number,
  ) {
    const fmt = this.trace.timeline.timestampFormat;
    switch (fmt) {
      case TimestampFormat.UTC:
      case TimestampFormat.CustomTimezone:
      case TimestampFormat.TraceTz:
      case TimestampFormat.Timecode:
        return renderTimecode(ctx, time, x, y, minWidth);
      case TimestampFormat.TraceNs:
        return renderRawTimestamp(ctx, time.toString(), x, y, minWidth);
      case TimestampFormat.TraceNsLocale:
        return renderRawTimestamp(ctx, time.toLocaleString(), x, y, minWidth);
      case TimestampFormat.Seconds:
        return renderRawTimestamp(
          ctx,
          Time.formatSeconds(time),
          x,
          y,
          minWidth,
        );
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
}

function formatRawTimestamp(
  fmt:
    | TimestampFormat.TraceNs
    | TimestampFormat.TraceNsLocale
    | TimestampFormat.Seconds
    | TimestampFormat.Milliseconds
    | TimestampFormat.Microseconds,
  time: time,
): string {
  switch (fmt) {
    case TimestampFormat.TraceNs:
      return time.toString();
    case TimestampFormat.TraceNsLocale:
      return time.toLocaleString();
    case TimestampFormat.Seconds:
      return Time.formatSeconds(time);
    case TimestampFormat.Milliseconds:
      return Time.formatMilliseconds(time);
    case TimestampFormat.Microseconds:
      return Time.formatMicroseconds(time);
    default:
      assertUnreachable(fmt);
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
  ctx.font = `11px ${FONT_COMPACT}`;
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
  ctx.font = `11px ${FONT_COMPACT}`;

  const {dhhmmss} = timecode;
  const thinSpace = '\u2009';
  const subsec = timecode.subsec(thinSpace);
  ctx.fillText(dhhmmss, x, y, minWidth);
  const {width: firstRowWidth} = ctx.measureText(subsec);

  ctx.font = `10.5px 10px ${FONT_COMPACT}`;
  ctx.fillText(subsec, x, y + 10, minWidth);
  const {width: secondRowWidth} = ctx.measureText(subsec);

  return Math.max(firstRowWidth, secondRowWidth);
}
