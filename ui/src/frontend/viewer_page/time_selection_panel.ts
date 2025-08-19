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
import {assertUnreachable} from '../../base/logging';
import {time, Time} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {formatDuration} from '../../components/time_utils';
import {TraceImpl} from '../../core/trace_impl';
import {TimestampFormat} from '../../public/timeline';
import {
  COLOR_BACKGROUND,
  FONT_COMPACT,
  COLOR_TEXT_MUTED,
  COLOR_BORDER,
  TRACK_SHELL_WIDTH,
  COLOR_TEXT,
} from '../css_constants';
import {generateTicks, getMaxMajorTicks, TickType} from './gridline_helper';

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
  ctx: CanvasRenderingContext2D,
  target: BBox,
  bounds: BBox,
  label: string,
) {
  ctx.fillStyle = COLOR_TEXT_MUTED;

  const xLeft = Math.floor(target.x);
  const xRight = Math.floor(target.x + target.width);
  const yMid = Math.floor(target.height / 2 + target.y);
  const xWidth = xRight - xLeft;

  // Don't draw in the track shell.
  ctx.beginPath();
  ctx.rect(bounds.x, bounds.y, bounds.width, bounds.height);
  ctx.clip();

  // Draw horizontal bar of the H.
  ctx.fillRect(xLeft, yMid, xWidth, 1);
  // Draw left vertical bar of the H.
  ctx.fillRect(xLeft, target.y, 1, target.height);
  // Draw right vertical bar of the H.
  ctx.fillRect(xRight, target.y, 1, target.height);

  const labelWidth = ctx.measureText(label).width;

  // Find a good position for the label:
  // By default put the label in the middle of the visible portion of the H.
  const visibleLeft = Math.max(xLeft, bounds.x);
  const visibleRight = Math.min(xRight, bounds.x + bounds.width);
  const visibleCenter = Math.floor((visibleLeft + visibleRight) / 2);
  let labelXLeft = Math.floor(visibleCenter - labelWidth / 2);

  if (
    labelWidth > target.width ||
    labelXLeft < bounds.x ||
    labelXLeft + labelWidth > bounds.x + bounds.width
  ) {
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

  ctx.fillStyle = COLOR_BACKGROUND;
  ctx.fillRect(labelXLeft - 1, 0, labelWidth + 1, target.height);

  ctx.textBaseline = 'middle';
  ctx.fillStyle = COLOR_TEXT_MUTED;
  ctx.font = `10px ${FONT_COMPACT}`;
  ctx.fillText(label, labelXLeft, yMid);
}

function drawIBar(
  ctx: CanvasRenderingContext2D,
  xPos: number,
  bounds: BBox,
  label: string,
) {
  if (xPos < bounds.x) return;

  ctx.fillStyle = COLOR_TEXT_MUTED;
  ctx.fillRect(xPos, 0, 1, bounds.width);

  const yMid = Math.floor(bounds.height / 2 + bounds.y);
  const labelWidth = ctx.measureText(label).width;
  const padding = 3;

  let xPosLabel;
  if (xPos + padding + labelWidth > bounds.width) {
    xPosLabel = xPos - padding;
    ctx.textAlign = 'right';
  } else {
    xPosLabel = xPos + padding;
    ctx.textAlign = 'left';
  }

  ctx.fillStyle = COLOR_BACKGROUND;
  ctx.fillRect(xPosLabel - 1, 0, labelWidth + 2, bounds.height);

  ctx.textBaseline = 'middle';
  ctx.fillStyle = COLOR_TEXT_MUTED;
  ctx.font = `10px ${FONT_COMPACT}`;
  ctx.fillText(label, xPosLabel, yMid);
}

// Draws a marker: triangle pointing down.
function drawMarker(ctx: CanvasRenderingContext2D, target: BBox, bounds: BBox) {
  const xPos = Math.floor(target.x);
  if (xPos < bounds.x || xPos > bounds.x + bounds.width) return;

  ctx.fillStyle = COLOR_TEXT;
  const yMid = Math.floor(target.height / 2 + target.y);
  const size = 4;

  // Don't draw in the track shell.
  ctx.beginPath();
  ctx.rect(bounds.x, bounds.y, bounds.width, bounds.height);
  ctx.clip();

  // Draw triangle pointing down. Offset it down a bit to balance triange being top-heavy.
  const yCenter = yMid + 1;
  ctx.beginPath();
  ctx.moveTo(xPos - size, yCenter - size);
  ctx.lineTo(xPos + size, yCenter - size);
  ctx.lineTo(xPos, yCenter + size);
  ctx.closePath();
  ctx.fill();
}

export class TimeSelectionPanel {
  readonly height = 10;

  constructor(private readonly trace: TraceImpl) {}

  render(): m.Children {
    return m('', {style: {height: `${this.height}px`}});
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: Size2D) {
    ctx.fillStyle = COLOR_BORDER;
    ctx.fillRect(TRACK_SHELL_WIDTH - 1, 0, 1, size.height);

    const trackSize = {...size, width: size.width - TRACK_SHELL_WIDTH};

    ctx.save();
    ctx.translate(TRACK_SHELL_WIDTH, 0);
    canvasClip(ctx, 0, 0, trackSize.width, trackSize.height);
    this.renderPanel(ctx, trackSize);
    ctx.restore();
  }

  private renderPanel(ctx: CanvasRenderingContext2D, size: Size2D): void {
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

    const localSpan = this.trace.timeline.selectedSpan;
    const selection = this.trace.selection.selection;
    if (localSpan !== undefined) {
      const start = Time.min(localSpan.start, localSpan.end);
      const end = Time.max(localSpan.start, localSpan.end);
      this.renderSpan(ctx, timescale, size, start, end);
    } else {
      if (selection.kind === 'area') {
        const start = Time.min(selection.start, selection.end);
        const end = Time.max(selection.start, selection.end);
        this.renderSpan(ctx, timescale, size, start, end);
      } else if (
        selection.kind === 'track_event' &&
        selection.dur !== undefined
      ) {
        const start = selection.ts;
        const end = Time.add(selection.ts, selection.dur);
        if (selection.dur === 0n) {
          this.renderInstantEvent(ctx, timescale, size, selection.ts);
        } else if (end > start) {
          this.renderSpan(ctx, timescale, size, start, end);
        }
      }
    }

    if (this.trace.timeline.hoverCursorTimestamp !== undefined) {
      this.renderHover(
        ctx,
        timescale,
        size,
        this.trace.timeline.hoverCursorTimestamp,
      );
    }

    for (const note of this.trace.notes.notes.values()) {
      const noteIsSelected =
        selection.kind === 'note' && selection.id === note.id;
      if (note.noteType === 'SPAN' && noteIsSelected) {
        this.renderSpan(ctx, timescale, size, note.start, note.end);
      }
    }

    ctx.restore();
  }

  renderHover(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    size: Size2D,
    ts: time,
  ) {
    const xPos = Math.floor(timescale.timeToPx(ts));
    const bounds = this.getBBoxFromSize(size);

    const hoverVisible = bounds.x <= xPos && xPos <= bounds.x + bounds.width;

    if (hoverVisible) {
      const domainTime = this.trace.timeline.toDomainTime(ts);
      const label = this.stringifyTimestamp(domainTime);
      drawIBar(ctx, xPos, bounds, label);
      return;
    }

    ctx.save();
    ctx.font = `10px ${FONT_COMPACT}`;
    ctx.textBaseline = 'middle';

    const yMid = Math.floor(bounds.height / 2);

    const {label, labelWidth, textX} = (() => {
      if (xPos < bounds.x) {
        const distance = Time.sub(timescale.timeSpan.start.toTime(), ts);
        const label = `← ${formatDuration(this.trace, distance)}`;
        const labelWidth = ctx.measureText(label).width;
        return {
          textX: bounds.x,
          label,
          labelWidth,
        };
      } else {
        const distance = Time.sub(ts, timescale.timeSpan.end.toTime());
        const label = `${formatDuration(this.trace, distance)} →`;
        const labelWidth = ctx.measureText(label).width;
        return {
          label,
          labelWidth,
          textX: bounds.x + bounds.width - labelWidth,
        };
      }
    })();

    ctx.fillStyle = COLOR_BACKGROUND;
    ctx.fillRect(textX - 1, 0, labelWidth + 2, bounds.height);
    ctx.fillStyle = COLOR_TEXT;
    ctx.fillText(label, textX, yMid);
    ctx.restore();
  }

  renderSpan(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    trackSize: Size2D,
    start: time,
    end: time,
  ) {
    const xLeft = timescale.timeToPx(start);
    const xRight = timescale.timeToPx(end);
    const label = formatDuration(this.trace, end - start);
    drawHBar(
      ctx,
      {
        x: xLeft,
        y: 0,
        width: xRight - xLeft,
        height: trackSize.height,
      },
      this.getBBoxFromSize(trackSize),
      label,
    );
  }

  renderInstantEvent(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    trackSize: Size2D,
    ts: time,
  ) {
    const xPos = timescale.timeToPx(ts);
    drawMarker(
      ctx,
      {
        x: xPos,
        y: 0,
        width: 0,
        height: trackSize.height,
      },
      this.getBBoxFromSize(trackSize),
    );
  }

  private getBBoxFromSize(size: Size2D): BBox {
    return {
      x: 0,
      y: 0,
      width: size.width,
      height: size.height,
    };
  }

  private stringifyTimestamp(time: time): string {
    const fmt = this.trace.timeline.timestampFormat;
    switch (fmt) {
      case TimestampFormat.UTC:
      case TimestampFormat.CustomTimezone:
      case TimestampFormat.TraceTz:
      case TimestampFormat.Timecode:
        const THIN_SPACE = '\u2009';
        return Time.toTimecode(time).toString(THIN_SPACE);
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
}
