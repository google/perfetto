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
import {Duration, duration, Time, time, TimeSpan} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {getOrCreate} from '../../base/utils';
import {ZonedInteractionHandler} from '../../base/zoned_interaction_handler';
import {colorForCpu} from '../../components/colorizer';
import {raf} from '../../core/raf_scheduler';
import {TraceImpl} from '../../core/trace_impl';
import {TimestampFormat} from '../../public/timeline';
import {LONG, NUM} from '../../trace_processor/query_result';
import {VirtualOverlayCanvas} from '../../widgets/virtual_overlay_canvas';
import {OVERVIEW_TIMELINE_NON_VISIBLE_COLOR} from '../css_constants';
import {
  generateTicks,
  getMaxMajorTicks,
  MIN_PX_PER_STEP,
  TickType,
} from './gridline_helper';

const HANDLE_SIZE_PX = 5;

export interface OverviewTimelineAttrs {
  readonly trace: TraceImpl;
  readonly className?: string;
}

const tracesData = new WeakMap<TraceImpl, OverviewDataLoader>();

export class OverviewTimeline
  implements m.ClassComponent<OverviewTimelineAttrs>
{
  private readonly overviewData: OverviewDataLoader;
  private readonly trash = new DisposableStack();
  private interactions?: ZonedInteractionHandler;

  constructor({attrs}: m.CVnode<OverviewTimelineAttrs>) {
    this.overviewData = getOrCreate(
      tracesData,
      attrs.trace,
      () => new OverviewDataLoader(attrs.trace),
    );
  }

  view({attrs}: m.CVnode<OverviewTimelineAttrs>) {
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

  oncreate({dom}: m.VnodeDOM<OverviewTimelineAttrs, this>) {
    this.interactions = new ZonedInteractionHandler(toHTMLElement(dom));
    this.trash.use(this.interactions);
  }

  onremove(_: m.VnodeDOM<OverviewTimelineAttrs, this>) {
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
      const offset = trace.timeline.timestampOffset();
      const tickGen = generateTicks(traceContext, maxMajorTicks, offset);

      // Draw time labels
      ctx.font = '10px Roboto Condensed';
      ctx.fillStyle = '#999';
      for (const {type, time} of tickGen) {
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

    // Draw mini-tracks with quanitzed density for each process.
    const overviewData = this.overviewData.overviewData;
    if (overviewData.size > 0) {
      const numTracks = overviewData.size;
      let y = 0;
      const trackHeight = (tracksHeight - 1) / numTracks;
      for (const key of overviewData.keys()) {
        const loads = overviewData.get(key)!;
        for (let i = 0; i < loads.length; i++) {
          const xStart = Math.floor(timescale.timeToPx(loads[i].start));
          const xEnd = Math.ceil(timescale.timeToPx(loads[i].end));
          const yOff = Math.floor(headerHeight + y * trackHeight);
          const lightness = Math.ceil((1 - loads[i].load * 0.7) * 100);
          const color = colorForCpu(y).setHSL({s: 50, l: lightness});
          ctx.fillStyle = color.cssString;
          ctx.fillRect(xStart, yOff, xEnd - xStart, Math.ceil(trackHeight));
        }
        y++;
      }
    }

    // Draw bottom border.
    ctx.fillStyle = '#dadada';
    ctx.fillRect(0, size.height - 1, size.width, 1);

    // Draw semi-opaque rects that occlude the non-visible time range.
    const {left, right} = timescale.hpTimeSpanToPxSpan(
      trace.timeline.visibleWindow,
    );

    const vizStartPx = Math.floor(left);
    const vizEndPx = Math.ceil(right);

    ctx.fillStyle = OVERVIEW_TIMELINE_NON_VISIBLE_COLOR;
    ctx.fillRect(0, headerHeight, vizStartPx, tracksHeight);
    ctx.fillRect(vizEndPx, headerHeight, size.width - vizEndPx, tracksHeight);

    // Draw brushes.
    ctx.fillStyle = '#999';
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
            trace.timeline.panVisibleWindow(delta);
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
            trace.timeline.updateVisibleTimeHP(span);
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
  const fmt = trace.timeline.timestampFormat;
  switch (fmt) {
    case TimestampFormat.UTC:
    case TimestampFormat.TraceTz:
    case TimestampFormat.Timecode:
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

interface QuantizedLoad {
  start: time;
  end: time;
  load: number;
}

// Kicks of a sequence of promises that load the overiew data in steps.
// Each step schedules an animation frame.
class OverviewDataLoader {
  overviewData = new Map<string, QuantizedLoad[]>();

  constructor(private trace: TraceImpl) {
    this.beginLoad();
  }

  async beginLoad() {
    const traceSpan = new TimeSpan(
      this.trace.traceInfo.start,
      this.trace.traceInfo.end,
    );
    const engine = this.trace.engine;
    const stepSize = Duration.max(1n, traceSpan.duration / 100n);
    const hasSchedSql = 'select ts from sched limit 1';
    const hasSchedOverview = (await engine.query(hasSchedSql)).numRows() > 0;
    if (hasSchedOverview) {
      await this.loadSchedOverview(traceSpan, stepSize);
    } else {
      await this.loadSliceOverview(traceSpan, stepSize);
    }
  }

  async loadSchedOverview(traceSpan: TimeSpan, stepSize: duration) {
    const stepPromises = [];
    for (
      let start = traceSpan.start;
      start < traceSpan.end;
      start = Time.add(start, stepSize)
    ) {
      const progress = start - traceSpan.start;
      const ratio = Number(progress) / Number(traceSpan.duration);
      this.trace.omnibox.showStatusMessage(
        'Loading overview ' + `${Math.round(ratio * 100)}%`,
      );
      const end = Time.add(start, stepSize);
      // The (async() => {})() queues all the 100 async promises in one batch.
      // Without that, we would wait for each step to be rendered before
      // kicking off the next one. That would interleave an animation frame
      // between each step, slowing down significantly the overall process.
      stepPromises.push(
        (async () => {
          const schedResult = await this.trace.engine.query(`
            select
              cast(sum(dur) as float)/${stepSize} as load,
              cpu from sched
            where
              ts >= ${start} and
              ts < ${end} and
              not utid in (select utid from thread where is_idle)
            group by cpu
            order by cpu
          `);
          const schedData: {[key: string]: QuantizedLoad} = {};
          const it = schedResult.iter({load: NUM, cpu: NUM});
          for (; it.valid(); it.next()) {
            const load = it.load;
            const cpu = it.cpu;
            schedData[cpu] = {start, end, load};
          }
          this.appendData(schedData);
        })(),
      );
    } // for(start = ...)
    await Promise.all(stepPromises);
  }

  async loadSliceOverview(traceSpan: TimeSpan, stepSize: duration) {
    // Slices overview.
    const sliceResult = await this.trace.engine.query(`
      select
        bucket,
        upid,
        ifnull(sum(utid_sum) / cast(${stepSize} as float), 0) as load
      from thread
      inner join (
        select
          ifnull(cast((ts - ${traceSpan.start})/${stepSize} as int), 0) as bucket,
          sum(dur) as utid_sum,
          utid
        from slice
        inner join thread_track on slice.track_id = thread_track.id
        group by bucket, utid
      ) using(utid)
      where upid is not null
      group by bucket, upid
    `);

    const slicesData: {[key: string]: QuantizedLoad[]} = {};
    const it = sliceResult.iter({bucket: LONG, upid: NUM, load: NUM});
    for (; it.valid(); it.next()) {
      const bucket = it.bucket;
      const upid = it.upid;
      const load = it.load;

      const start = Time.add(traceSpan.start, stepSize * bucket);
      const end = Time.add(start, stepSize);

      const upidStr = upid.toString();
      let loadArray = slicesData[upidStr];
      if (loadArray === undefined) {
        loadArray = slicesData[upidStr] = [];
      }
      loadArray.push({start, end, load});
    }
    this.appendData(slicesData);
  }

  appendData(data: {[key: string]: QuantizedLoad | QuantizedLoad[]}) {
    for (const [key, value] of Object.entries(data)) {
      if (!this.overviewData.has(key)) {
        this.overviewData.set(key, []);
      }
      if (value instanceof Array) {
        this.overviewData.get(key)!.push(...value);
      } else {
        this.overviewData.get(key)!.push(value);
      }
    }
    raf.scheduleCanvasRedraw();
  }
}
