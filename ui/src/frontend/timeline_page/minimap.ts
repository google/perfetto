// Copyright (C) 2024 The Android Open Source Project
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

import m from 'mithril';
import {DisposableStack} from '../../base/disposable_stack';
import {toHTMLElement} from '../../base/dom_utils';
import {Rect2D, Size2D} from '../../base/geom';
import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {assertExists, assertUnreachable} from '../../base/logging';
import {Time, time, TimeSpan} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {ZonedInteractionHandler} from '../../base/zoned_interaction_handler';
import {colorForCpu} from '../../components/colorizer';
import {TraceImpl} from '../../core/trace_impl';
import {TimestampFormat} from '../../public/timeline';
import {VirtualOverlayCanvas} from '../../widgets/virtual_overlay_canvas';
import {
  COLOR_TEXT_MUTED,
  FONT_COMPACT,
  COLOR_BORDER,
  COLOR_NEUTRAL,
} from '../css_constants';
import {
  generateTicks,
  getMaxMajorTicks,
  MIN_PX_PER_STEP,
  TickType,
} from './gridline_helper';

const HANDLE_SIZE_PX = 5;

export interface MinimapAttrs {
  readonly trace: TraceImpl;
  readonly className?: string;
}

export class Minimap implements m.ClassComponent<MinimapAttrs> {
  private readonly trash = new DisposableStack();
  private interactions?: ZonedInteractionHandler;

  view({attrs}: m.CVnode<MinimapAttrs>) {
    return m(
      VirtualOverlayCanvas,
      {
        onMount: (redrawCanvas) =>
          attrs.trace.raf.addCanvasRedrawCallback(redrawCanvas),
        disableCanvasRedrawOnMithrilUpdates: true,
        className: attrs.className,
        onCanvasRedraw: ({ctx, virtualCanvasSize}) => {
          this.renderCanvas(attrs.trace, ctx, virtualCanvasSize);
        },
      },
      m('.pf-overview-timeline'),
    );
  }

  oncreate({dom}: m.VnodeDOM<MinimapAttrs, this>) {
    this.interactions = new ZonedInteractionHandler(toHTMLElement(dom));
    this.trash.use(this.interactions);
  }

  onremove(_: m.VnodeDOM<MinimapAttrs, this>) {
    this.trash.dispose();
  }

  private renderCanvas(
    trace: TraceImpl,
    ctx: CanvasRenderingContext2D,
    size: Size2D,
  ) {
    if (size.width <= 0) return;

    const traceTime = trace.traceInfo;
    const pxBounds = {left: 0, right: size.width};
    const hpTraceTime = HighPrecisionTimeSpan.fromTime(
      traceTime.start,
      traceTime.end,
    );
    const timescale = new TimeScale(hpTraceTime, pxBounds);

    const headerHeight = 20;
    const tracksHeight = size.height - headerHeight;
    const traceContext = new TimeSpan(
      trace.traceInfo.start,
      trace.traceInfo.end,
    );

    if (size.width > 0 && traceContext.duration > 0n) {
      const maxMajorTicks = getMaxMajorTicks(size.width);
      const offset = trace.timeline.getTimeAxisOrigin();
      const tickGen = generateTicks(traceContext, maxMajorTicks, offset);

      // Draw time labels
      ctx.font = `10px ${FONT_COMPACT}`;
      for (const {type, time} of tickGen) {
        ctx.fillStyle = COLOR_BORDER;
        const xPos = Math.floor(timescale.timeToPx(time));
        if (xPos <= 0) continue;
        if (xPos > size.width) break;
        if (type === TickType.MAJOR) {
          ctx.fillRect(xPos - 1, 0, 1, headerHeight - 5);
          const domainTime = trace.timeline.toDomainTime(time);
          renderTimestamp(
            trace,
            ctx,
            domainTime,
            xPos + 5,
            18,
            MIN_PX_PER_STEP,
          );
        } else if (type == TickType.MEDIUM) {
          ctx.fillRect(xPos - 1, 0, 1, 8);
        } else if (type == TickType.MINOR) {
          ctx.fillRect(xPos - 1, 0, 1, 5);
        }
      }
    }

    // Render the minimap data
    const rows = trace.minimap.getLoad();
    if (rows) {
      const numTracks = rows.length;
      const trackHeight = (tracksHeight - 1) / numTracks;
      let y = 0;
      for (const row of rows) {
        for (const cell of row) {
          const x = Math.floor(timescale.timeToPx(cell.ts));
          const width = Math.ceil(timescale.durationToPx(cell.dur));
          const yOff = Math.floor(headerHeight + y * trackHeight);
          const color = colorForCpu(y).setHSL({s: 50}).setAlpha(cell.load);
          ctx.fillStyle = color.cssString;
          ctx.clearRect(x, yOff, width, Math.ceil(trackHeight));
          ctx.fillRect(x, yOff, width, Math.ceil(trackHeight));
        }
        y++;
      }
    }

    // Draw bottom border.
    // ctx.fillStyle = '#dadada';
    ctx.fillRect(0, size.height - 1, size.width, 1);

    // Draw semi-opaque rects that occlude the non-visible time range.
    const {left, right} = timescale.hpTimeSpanToPxSpan(
      trace.timeline.visibleWindow,
    );

    const vizStartPx = Math.floor(left);
    const vizEndPx = Math.ceil(right);

    ctx.globalAlpha = 0.5;
    ctx.fillStyle = COLOR_NEUTRAL;
    ctx.fillRect(0, headerHeight, vizStartPx, tracksHeight);
    ctx.fillRect(vizEndPx, headerHeight, size.width - vizEndPx, tracksHeight);
    ctx.globalAlpha = 1.0;

    // Draw brushes.
    ctx.fillStyle = COLOR_BORDER;
    ctx.fillRect(vizStartPx - 1, headerHeight, 1, tracksHeight);
    ctx.fillRect(vizEndPx, headerHeight, 1, tracksHeight);

    const hbarWidth = HANDLE_SIZE_PX;
    const hbarHeight = tracksHeight * 0.4;
    // Draw handlebar
    ctx.fillRect(
      vizStartPx - Math.floor(hbarWidth / 2) - 1,
      headerHeight,
      hbarWidth,
      hbarHeight,
    );
    ctx.fillRect(
      vizEndPx - Math.floor(hbarWidth / 2),
      headerHeight,
      hbarWidth,
      hbarHeight,
    );

    assertExists(this.interactions).update([
      {
        id: 'left-handle',
        area: Rect2D.fromPointAndSize({
          x: vizStartPx - Math.floor(hbarWidth / 2) - 1,
          y: 0,
          width: hbarWidth,
          height: size.height,
        }),
        cursor: 'col-resize',
        drag: {
          cursorWhileDragging: 'col-resize',
          onDrag: (event) => {
            const delta = timescale.pxToDuration(event.deltaSinceLastEvent.x);
            trace.timeline.moveStart(delta);
          },
        },
      },
      {
        id: 'right-handle',
        area: Rect2D.fromPointAndSize({
          x: vizEndPx - Math.floor(hbarWidth / 2) - 1,
          y: 0,
          width: hbarWidth,
          height: size.height,
        }),
        cursor: 'col-resize',
        drag: {
          cursorWhileDragging: 'col-resize',
          onDrag: (event) => {
            const delta = timescale.pxToDuration(event.deltaSinceLastEvent.x);
            trace.timeline.moveEnd(delta);
          },
        },
      },
      {
        id: 'drag',
        area: new Rect2D({
          left: vizStartPx,
          right: vizEndPx,
          top: 0,
          bottom: size.height,
        }),
        cursor: 'grab',
        drag: {
          cursorWhileDragging: 'grabbing',
          onDrag: (event) => {
            const delta = timescale.pxToDuration(event.deltaSinceLastEvent.x);
            trace.timeline.pan(delta);
          },
        },
      },
      {
        id: 'select',
        area: new Rect2D({
          left: 0,
          right: size.width,
          top: 0,
          bottom: size.height,
        }),
        cursor: 'text',
        drag: {
          cursorWhileDragging: 'text',
          onDrag: (event) => {
            const span = timescale.pxSpanToHpTimeSpan(
              Rect2D.fromPoints(event.dragStart, event.dragCurrent),
            );
            trace.timeline.setVisibleWindow(span);
          },
        },
      },
    ]);
  }
}

// Print a timestamp in the configured time format
function renderTimestamp(
  trace: TraceImpl,
  ctx: CanvasRenderingContext2D,
  time: time,
  x: number,
  y: number,
  minWidth: number,
): void {
  ctx.fillStyle = COLOR_TEXT_MUTED;
  const fmt = trace.timeline.timestampFormat;
  switch (fmt) {
    case TimestampFormat.UTC:
    case TimestampFormat.TraceTz:
    case TimestampFormat.Timecode:
    case TimestampFormat.CustomTimezone:
      renderTimecode(ctx, time, x, y, minWidth);
      break;
    case TimestampFormat.TraceNs:
      ctx.fillText(time.toString(), x, y, minWidth);
      break;
    case TimestampFormat.TraceNsLocale:
      ctx.fillText(time.toLocaleString(), x, y, minWidth);
      break;
    case TimestampFormat.Seconds:
      ctx.fillText(Time.formatSeconds(time), x, y, minWidth);
      break;
    case TimestampFormat.Milliseconds:
      ctx.fillText(Time.formatMilliseconds(time), x, y, minWidth);
      break;
    case TimestampFormat.Microseconds:
      ctx.fillText(Time.formatMicroseconds(time), x, y, minWidth);
      break;
    default:
      assertUnreachable(fmt);
  }
}

// Print a timecode over 2 lines with this formatting:
// DdHH:MM:SS
// mmm uuu nnn
function renderTimecode(
  ctx: CanvasRenderingContext2D,
  time: time,
  x: number,
  y: number,
  minWidth: number,
): void {
  const timecode = Time.toTimecode(time);
  const {dhhmmss} = timecode;
  ctx.fillText(dhhmmss, x, y, minWidth);
}
