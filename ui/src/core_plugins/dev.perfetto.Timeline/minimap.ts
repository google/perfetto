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
import {
  assertUnreachable,
  ensureExists,
  ensureIsInstance,
} from '../../base/assert';
import {canvasClip, canvasSave} from '../../base/canvas_utils';
import {findRef} from '../../base/dom_utils';
import {Rect2D, type Size2D} from '../../base/geom';
import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {startDragGesture} from '../../base/mithril_utils';
import {Icons} from '../../base/semantic_icons';
import {Time, type time, TimeSpan} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {colorForCpu} from '../../components/colorizer';
import type {TraceImpl} from '../../core/trace_impl';
import {GUTTER_FRACTION} from '../../core/timeline';
import {
  COLOR_BORDER,
  COLOR_TEXT_MUTED,
  FONT_COMPACT,
} from '../../frontend/css_constants';
import {TimestampFormat} from '../../public/timeline';
import {Button} from '../../widgets/button';
import {CursorTooltip} from '../../widgets/cursor_tooltip';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {VirtualOverlayCanvas} from '../../widgets/virtual_overlay_canvas';
import {
  generateTicks,
  getMaxMajorTicks,
  MIN_PX_PER_STEP,
  TickType,
} from './gridline_helper';
import './minimap.scss';

const HEADER_HEIGHT_PX = 20;
const HANDLE_SIZE_PX = 5;
// Pointer movement below which a press on the minimap counts as a click.
const CLICK_DEADZONE_PX = 3;

export interface MinimapAttrs {
  readonly trace: TraceImpl;
  readonly className?: string;
}

const MINIMAP_REF = 'minimap';

// Maps the whole trace onto a minimap of the given width, leaving a gutter at
// each end so the viewport box and brush handles can be dragged slightly past
// the trace start/end, matching the main timeline.
function traceTimescale(trace: TraceImpl, widthPx: number): TimeScale {
  const gutter = widthPx * GUTTER_FRACTION;
  return new TimeScale(
    HighPrecisionTimeSpan.fromTime(trace.traceInfo.start, trace.traceInfo.end),
    {left: gutter, right: widthPx - gutter},
  );
}

function pxToTraceTime(trace: TraceImpl, timescale: TimeScale, x: number) {
  return Time.clamp(
    timescale.pxToHpTime(x).toTime(),
    trace.traceInfo.start,
    trace.traceInfo.end,
  );
}

export class Minimap implements m.ClassComponent<MinimapAttrs> {
  // The hovered timestamp, only tracked if the active mode has a tooltip.
  private hoveredTs?: time;

  view({attrs}: m.CVnode<MinimapAttrs>) {
    const {trace} = attrs;
    const tooltip =
      this.hoveredTs === undefined
        ? undefined
        : trace.minimap.activeMode?.renderTooltip?.(this.hoveredTs);
    return m(
      VirtualOverlayCanvas,
      {
        onMount: ({redrawCanvas}) =>
          trace.raf.addCanvasRedrawCallback(redrawCanvas),
        disableCanvasRedrawOnMithrilUpdates: true,
        className: attrs.className,
        onCanvasRedraw: ({ctx, virtualCanvasSize, dom}) => {
          this.renderCanvas(trace, ctx, virtualCanvasSize, dom);
        },
      },
      m(
        '.pf-minimap',
        {
          style: {height: `${trace.minimap.heightPx}px`},
          onpointermove: (e: PointerEvent) => this.onHover(trace, e),
          onpointerleave: () => this.onHover(trace, undefined),
        },
        m('.pf-minimap__overlay', {ref: MINIMAP_REF}),
        trace.minimap.modes.length > 1 && this.renderMenu(trace),
      ),
      tooltip !== undefined && m(CursorTooltip, tooltip),
    );
  }

  // Tracks the hovered timestamp for the active mode's tooltip. Pass undefined
  // when the pointer leaves the minimap.
  private onHover(trace: TraceImpl, e: PointerEvent | undefined) {
    const hasTooltip = trace.minimap.activeMode?.renderTooltip !== undefined;
    // Don't show the tooltip while dragging (i.e. a button is pressed).
    if (e === undefined || e.buttons !== 0 || !hasTooltip) {
      if (this.hoveredTs !== undefined) {
        this.hoveredTs = undefined;
        trace.timeline.hoverCursorTimestamp = undefined;
      }
      return;
    }
    const rect = ensureIsInstance(
      e.currentTarget,
      HTMLElement,
    ).getBoundingClientRect();
    const timescale = traceTimescale(trace, rect.width);
    this.hoveredTs = pxToTraceTime(trace, timescale, e.clientX - rect.left);
    trace.timeline.hoverCursorTimestamp = this.hoveredTs;
  }

  private renderMenu(trace: TraceImpl): m.Children {
    const activeId = trace.minimap.activeModeId;
    return m(
      '.pf-minimap__menu',
      {
        // Don't show the hover tooltip while interacting with the menu.
        onpointermove: (e: PointerEvent) => {
          e.stopPropagation();
          this.onHover(trace, undefined);
        },
      },
      m(
        PopupMenu,
        {
          trigger: m(Button, {
            className: 'pf-visible-on-hover',
            icon: Icons.ContextMenuAlt,
            compact: true,
            title: 'Minimap mode',
          }),
        },
        trace.minimap.modes.map((mode) =>
          m(MenuItem, {
            label: mode.name,
            icon:
              mode.id === activeId ? Icons.RadioChecked : Icons.RadioUnchecked,
            onclick: () => {
              trace.minimap.activeModeId = mode.id;
              trace.raf.scheduleCanvasRedraw();
            },
          }),
        ),
      ),
    );
  }

  private renderBrushes(trace: TraceImpl, element: Element) {
    const timeline = ensureExists(findRef(element, MINIMAP_REF));
    const timelineWidth = timeline.getBoundingClientRect().width;
    const timescale = traceTimescale(trace, timelineWidth);
    const {left, right} = timescale.hpTimeSpanToPxSpan(
      trace.timeline.visibleWindow,
    );

    const mode = trace.minimap.activeMode;
    const onClick =
      mode?.onClick &&
      ((x: number) => mode.onClick?.(pxToTraceTime(trace, timescale, x)));

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
        onClick,
      }),
      m(DragHandle, {
        left,
        right,
        onDrag: (deltaX) => {
          const delta = timescale.pxToDuration(deltaX);
          trace.timeline.pan(delta);
        },
        onClick,
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

    const timescale = traceTimescale(trace, size.width);
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

    const mode = trace.minimap.activeMode;
    if (mode) {
      const rect = new Rect2D({
        left: 0,
        top: HEADER_HEIGHT_PX,
        right: size.width,
        bottom: size.height,
      });
      using _ = canvasSave(ctx);
      canvasClip(ctx, rect);
      mode.render(ctx, timescale, rect);
    } else {
      this.renderActivity(trace, ctx, timescale, tracksHeight);
    }

    this.renderBrushes(trace, dom);
  }

  // Renders the activity overview from the content providers' rows.
  private renderActivity(
    trace: TraceImpl,
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    tracksHeight: number,
  ) {
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
  onClick?(x: number): void;
}

interface SelectionAreaAttrs extends m.Attributes {
  onDrag(startX: number, currentX: number): void;
  onClick?(x: number): void;
}

function SelectionArea(): m.Component<SelectionAreaAttrs> {
  return {
    view({attrs}: m.Vnode<SelectionAreaAttrs>) {
      return m('div', {
        class: 'pf-minimap__selection-area',
        onpointerdown: (e: PointerEvent) => {
          const target = e.currentTarget as HTMLElement;
          const rect = target.getBoundingClientRect();
          const dragStartX = e.clientX - rect.left;
          const {onClick} = attrs;
          startDragGesture({
            e,
            deadzonePx: onClick ? CLICK_DEADZONE_PX : 0,
            onDrag: (ev) => {
              attrs.onDrag(dragStartX, ev.clientX - rect.left);
            },
            onDragFailed: () => onClick?.(dragStartX),
          });
        },
      });
    },
  };
}

function DragHandle(): m.Component<DragHandleAttrs> {
  return {
    view({attrs}: m.Vnode<DragHandleAttrs>) {
      return m('span.pf-minimap__drag-handle', {
        onpointerdown: (e: PointerEvent) => {
          let lastClientX = e.clientX;
          const {onClick} = attrs;
          const handle = e.currentTarget as HTMLElement;
          const parentRect = handle.offsetParent!.getBoundingClientRect();
          startDragGesture({
            e,
            deadzonePx: onClick ? CLICK_DEADZONE_PX : 0,
            onDrag: (ev) => {
              attrs.onDrag(ev.clientX - lastClientX);
              lastClientX = ev.clientX;
            },
            onDragFailed: () => onClick?.(e.clientX - parentRect.left),
          });
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
  return {
    view({attrs}: m.Vnode<BrushAttrs>) {
      return m(
        '.pf-minimap__brush',
        {
          onpointerdown: (e: PointerEvent) => {
            const brushElement = e.currentTarget as HTMLElement;
            const parentRect =
              brushElement.offsetParent!.getBoundingClientRect();
            startDragGesture({
              e,
              onDrag: (ev) => {
                const x = Math.max(
                  attrs.minX,
                  Math.min(attrs.maxX, ev.clientX - parentRect.left),
                );
                attrs.onDrag(x);
              },
            });
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
