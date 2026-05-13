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
import {Size2D} from '../../base/geom';
import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {assertExists, assertUnreachable} from '../../base/assert';
import {Time, time, TimeSpan} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {colorForCpu} from '../../components/colorizer';
import {TraceImpl} from '../../core/trace_impl';
import {TimestampFormat} from '../../public/timeline';
import {VirtualOverlayCanvas} from '../../widgets/virtual_overlay_canvas';
import {COLOR_TEXT_MUTED, FONT_COMPACT, COLOR_BORDER} from '../../frontend/css_constants';
import {
  generateTicks,
  getMaxMajorTicks,
  MIN_PX_PER_STEP,
  TickType,
} from './gridline_helper';
import {findRef} from '../../base/dom_utils';

const HEADER_HEIGHT_PX = 20;
const HANDLE_SIZE_PX = 5;

export interface MinimapAttrs {
  readonly trace: TraceImpl;
  readonly className?: string;
}

const MINIMAP_REF = 'minimap';

export class Minimap implements m.ClassComponent<MinimapAttrs> {
  view({attrs}: m.CVnode<MinimapAttrs>) {
    return m(
      VirtualOverlayCanvas,
      {
        onMount: (redrawCanvas) =>
          attrs.trace.raf.addCanvasRedrawCallback(redrawCanvas),
        disableCanvasRedrawOnMithrilUpdates: true,
        className: attrs.className,
        onCanvasRedraw: ({ctx, virtualCanvasSize, dom}) => {
          this.renderCanvas(attrs.trace, ctx, virtualCanvasSize, dom);
        },
      },
      m('.pf-minimap', {ref: MINIMAP_REF}),
    );
  }

  private renderBrushes(trace: TraceImpl, element: Element) {
    const timeline = assertExists(findRef(element, MINIMAP_REF));
    const timelineWidth = timeline.getBoundingClientRect().width;
    const traceTime = trace.traceInfo;
    const pxBounds = {left: 0, right: timelineWidth};
    const hpTraceTime = HighPrecisionTimeSpan.fromTime(
      traceTime.start,
      traceTime.end,
    );
    const timescale = new TimeScale(hpTraceTime, pxBounds);
    const {left, right} = timescale.hpTimeSpanToPxSpan(
      trace.timeline.visibleWindow,
    );

    m.render(timeline, [
      // Occlusion shades over non-visible time ranges.
      m('.pf-minimap__shade', {
        style: {left: 0, width: `${left}px`},
      }),
      m('.pf-minimap__shade', {
        style: {left: `${right}px`, right: 0},
      }),
      m(SelectionArea, {
        onDrag: (startX, currentX) => {
          const left = Math.min(startX, currentX);
          const right = Math.max(startX, currentX);
          const span = timescale.pxSpanToHpTimeSpan({left, right});
          trace.timeline.setVisibleWindow(span);
        },
      }),
      m(DragHandle, {
        left,
        right,
        onDrag: (deltaX) => {
          const delta = timescale.pxToDuration(deltaX);
          trace.timeline.pan(delta);
        },
      }),
      m(Brush, {
        x: left,
        minX: 0,
        maxX: right - 1,
        onDrag: (x) => {
          trace.timeline.moveStart(timescale.pxToHpTime(x));
        },
      }),
      m(Brush, {
        x: right,
        minX: left + 1,
        maxX: timelineWidth,
        onDrag: (x) => {
          trace.timeline.moveEnd(timescale.pxToHpTime(x));
        },
      }),
    ]);
  }

  private renderCanvas(
    trace: TraceImpl,
    ctx: CanvasRenderingContext2D,
    size: Size2D,
    dom: Element,
  ) {
    if (size.width <= 0) return;

    const traceTime = trace.traceInfo;
    const pxBounds = {left: 0, right: size.width};
    const hpTraceTime = HighPrecisionTimeSpan.fromTime(
      traceTime.start,
      traceTime.end,
    );
    const timescale = new TimeScale(hpTraceTime, pxBounds);

    const tracksHeight = size.height - HEADER_HEIGHT_PX;
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
          ctx.fillRect(xPos - 1, 0, 1, HEADER_HEIGHT_PX - 5);
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
      const trackHeight = tracksHeight / numTracks;

      let y = 0;
      for (const row of rows) {
        const topFloat = HEADER_HEIGHT_PX + y * trackHeight;
        const top = Math.round(topFloat);
        const bottom = Math.round(topFloat + trackHeight);
        ctx.fillStyle = colorForCpu(y).setHSL({s: 50}).cssString;

        for (const cell of row) {
          const x = Math.round(timescale.timeToPx(cell.ts));
          const xEnd = Math.round(
            timescale.timeToPx(Time.fromRaw(cell.ts + cell.dur)),
          );
          ctx.globalAlpha = cell.load;
          ctx.fillRect(x, top, xEnd - x, bottom - top);
        }
        y++;
      }
      ctx.globalAlpha = 1.0;
    }

    this.renderBrushes(trace, dom);
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

interface BrushAttrs extends m.Attributes {
  readonly x: number;
  readonly minX: number;
  readonly maxX: number;
  onDrag(x: number): void;
}

interface DragHandleAttrs extends m.Attributes {
  readonly left: number;
  readonly right: number;
  onDrag(deltaX: number): void;
}

interface SelectionAreaAttrs extends m.Attributes {
  onDrag(startX: number, currentX: number): void;
}

function SelectionArea(): m.Component<SelectionAreaAttrs> {
  let dragStartX: number | undefined;

  return {
    view({attrs}: m.Vnode<SelectionAreaAttrs>) {
      return m('div', {
        onpointerdown: (e: PointerEvent) => {
          const target = e.target as HTMLElement;
          target.setPointerCapture(e.pointerId);
          const rect = target.getBoundingClientRect();
          dragStartX = e.clientX - rect.left;
        },
        onpointerup: (e: PointerEvent) => {
          const target = e.target as HTMLElement;
          target.releasePointerCapture(e.pointerId);
          dragStartX = undefined;
        },
        onpointermove: (e: PointerEvent) => {
          if (dragStartX !== undefined) {
            const target = e.target as HTMLElement;
            const rect = target.getBoundingClientRect();
            const currentX = e.clientX - rect.left;
            attrs.onDrag(dragStartX, currentX);
          }
        },
        class: 'pf-minimap__selection-area',
      });
    },
  };
}

function DragHandle(): m.Component<DragHandleAttrs> {
  let lastClientX: number | undefined;

  return {
    view({attrs}: m.Vnode<DragHandleAttrs>) {
      return m('span.pf-minimap__drag-handle', {
        onpointerdown: (e: PointerEvent) => {
          const target = e.target as HTMLElement;
          target.setPointerCapture(e.pointerId);
          lastClientX = e.clientX;
        },
        onpointerup: (e: PointerEvent) => {
          const target = e.target as HTMLElement;
          target.releasePointerCapture(e.pointerId);
          lastClientX = undefined;
        },
        onpointermove: (e: PointerEvent) => {
          if (lastClientX !== undefined) {
            const deltaX = e.clientX - lastClientX;
            lastClientX = e.clientX;
            attrs.onDrag(deltaX);
          }
        },
        style: {
          position: 'absolute',
          top: 0,
          left: `${attrs.left}px`,
          width: `${attrs.right - attrs.left}px`,
          height: '100%',
        },
      });
    },
  };
}

function Brush(): m.Component<BrushAttrs> {
  let dragging = false;

  return {
    view({attrs}: m.Vnode<BrushAttrs>) {
      return m(
        '.pf-minimap__brush',
        {
          onpointerdown: (e: PointerEvent) => {
            const target = e.currentTarget as HTMLElement;
            target.setPointerCapture(e.pointerId);
            dragging = true;
          },
          onpointerup: (e: PointerEvent) => {
            const target = e.currentTarget as HTMLElement;
            target.releasePointerCapture(e.pointerId);
            dragging = false;
          },
          onpointermove: (e: PointerEvent) => {
            if (dragging) {
              const brushElement = e.currentTarget as HTMLElement;
              const parentRect =
                brushElement.offsetParent!.getBoundingClientRect();
              const x = Math.max(
                attrs.minX,
                Math.min(attrs.maxX, e.clientX - parentRect.left),
              );
              attrs.onDrag(x);
            }
          },
          style: {
            position: 'absolute',
            top: 0,
            left: `${attrs.x - HANDLE_SIZE_PX / 2}px`,
            width: `${HANDLE_SIZE_PX}px`,
            height: '100%',
          },
        },
        // Vertical brush line
        m('.pf-minimap__brush-line'),
        // Handlebar
        m('.pf-minimap__brush-handlebar'),
      );
    },
  };
}
