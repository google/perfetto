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

import {
  Duration,
  duration,
  Span,
  time,
  Time,
  TimeSpan,
} from '../base/time';
import {timestampFormat, TimestampFormat} from '../common/timestamp_format';

import {
  BACKGROUND_COLOR,
  FOREGROUND_COLOR,
  TRACK_SHELL_WIDTH,
} from './css_constants';
import {globals} from './globals';
import {
  getMaxMajorTicks,
  TickGenerator,
  TickType,
  timeScaleForVisibleWindow,
} from './gridline_helper';
import {Panel, PanelSize} from './panel';

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
    ctx: CanvasRenderingContext2D, target: BBox, bounds: BBox, label: string) {
  ctx.fillStyle = FOREGROUND_COLOR;

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
  // By default put the label in the middle of the H:
  let labelXLeft = Math.floor(xWidth / 2 - labelWidth / 2 + xLeft);

  if (labelWidth > target.width || labelXLeft < bounds.x ||
      (labelXLeft + labelWidth) > (bounds.x + bounds.width)) {
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

  ctx.fillStyle = BACKGROUND_COLOR;
  ctx.fillRect(labelXLeft - 1, 0, labelWidth + 1, target.height);

  ctx.textBaseline = 'middle';
  ctx.fillStyle = FOREGROUND_COLOR;
  ctx.font = '10px Roboto Condensed';
  ctx.fillText(label, labelXLeft, yMid);
}

function drawIBar(
    ctx: CanvasRenderingContext2D, xPos: number, bounds: BBox, label: string) {
  if (xPos < bounds.x) return;

  ctx.fillStyle = FOREGROUND_COLOR;
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

  ctx.fillStyle = BACKGROUND_COLOR;
  ctx.fillRect(xPosLabel - 1, 0, labelWidth + 2, bounds.height);

  ctx.textBaseline = 'middle';
  ctx.fillStyle = FOREGROUND_COLOR;
  ctx.font = '10px Roboto Condensed';
  ctx.fillText(label, xPosLabel, yMid);
}

export class TimeSelectionPanel extends Panel {
  view() {
    return m('.time-selection-panel');
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    ctx.fillStyle = '#999';
    ctx.fillRect(TRACK_SHELL_WIDTH - 2, 0, 2, size.height);

    ctx.save();
    ctx.beginPath();
    ctx.rect(TRACK_SHELL_WIDTH, 0, size.width - TRACK_SHELL_WIDTH, size.height);
    ctx.clip();

    const span = globals.frontendLocalState.visibleTimeSpan;
    if (size.width > TRACK_SHELL_WIDTH && span.duration > 0n) {
      const maxMajorTicks = getMaxMajorTicks(size.width - TRACK_SHELL_WIDTH);
      const map = timeScaleForVisibleWindow(TRACK_SHELL_WIDTH, size.width);

      const offset = globals.timestampOffset();
      const tickGen = new TickGenerator(span, maxMajorTicks, offset);
      for (const {type, time} of tickGen) {
        const px = Math.floor(map.timeToPx(time));
        if (type === TickType.MAJOR) {
          ctx.fillRect(px, 0, 1, size.height);
        }
      }
    }

    const localArea = globals.frontendLocalState.selectedArea;
    const selection = globals.state.currentSelection;
    if (localArea !== undefined) {
      const start = Time.min(localArea.start, localArea.end);
      const end = Time.max(localArea.start, localArea.end);
      this.renderSpan(ctx, size, new TimeSpan(start, end));
    } else if (selection !== null && selection.kind === 'AREA') {
      const selectedArea = globals.state.areas[selection.areaId];
      const start = Time.min(selectedArea.start, selectedArea.end);
      const end = Time.max(selectedArea.start, selectedArea.end);
      this.renderSpan(ctx, size, new TimeSpan(start, end));
    }

    if (globals.state.hoverCursorTimestamp !== -1n) {
      this.renderHover(ctx, size, globals.state.hoverCursorTimestamp);
    }

    for (const note of Object.values(globals.state.notes)) {
      const noteIsSelected = selection !== null && selection.kind === 'AREA' &&
          selection.noteId === note.id;
      if (note.noteType === 'AREA' && !noteIsSelected) {
        const selectedArea = globals.state.areas[note.areaId];
        this.renderSpan(
            ctx, size, new TimeSpan(selectedArea.start, selectedArea.end));
      }
    }

    ctx.restore();
  }

  renderHover(ctx: CanvasRenderingContext2D, size: PanelSize, ts: time) {
    const {visibleTimeScale} = globals.frontendLocalState;
    const xPos = TRACK_SHELL_WIDTH + Math.floor(visibleTimeScale.timeToPx(ts));
    const domainTime = globals.toDomainTime(ts);
    const label = stringifyTimestamp(domainTime);
    drawIBar(ctx, xPos, this.bounds(size), label);
  }

  renderSpan(
      ctx: CanvasRenderingContext2D, size: PanelSize,
      span: Span<time, duration>) {
    const {visibleTimeScale} = globals.frontendLocalState;
    const xLeft = visibleTimeScale.timeToPx(span.start);
    const xRight = visibleTimeScale.timeToPx(span.end);
    const label = Duration.humanise(span.duration);
    drawHBar(
        ctx,
        {
          x: TRACK_SHELL_WIDTH + xLeft,
          y: 0,
          width: xRight - xLeft,
          height: size.height,
        },
        this.bounds(size),
        label);
  }

  private bounds(size: PanelSize): BBox {
    return {
      x: TRACK_SHELL_WIDTH,
      y: 0,
      width: size.width - TRACK_SHELL_WIDTH,
      height: size.height,
    };
  }
}

function stringifyTimestamp(time: time): string {
  const fmt = timestampFormat();
  switch (fmt) {
    case TimestampFormat.UTC:
    case TimestampFormat.Timecode:
      const THIN_SPACE = '\u2009';
      return Time.toTimecode(time).toString(THIN_SPACE);
    case TimestampFormat.Raw:
      return time.toString();
    case TimestampFormat.RawLocale:
      return time.toLocaleString();
    case TimestampFormat.Seconds:
      return Time.formatSeconds(time);
    default:
      const z: never = fmt;
      throw new Error(`Invalid timestamp ${z}`);
  }
}
