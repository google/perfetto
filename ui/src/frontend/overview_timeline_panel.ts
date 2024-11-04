// Copyright (C) 2018 The Android Open Source Project
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
import {Duration, Time, TimeSpan, duration, time} from '../base/time';
import {colorForCpu} from '../public/lib/colorizer';
import {timestampFormat, TimestampFormat} from '../core/timestamp_format';
import {
  OVERVIEW_TIMELINE_NON_VISIBLE_COLOR,
  TRACK_SHELL_WIDTH,
} from './css_constants';
import {BorderDragStrategy} from './drag/border_drag_strategy';
import {DragStrategy} from './drag/drag_strategy';
import {InnerDragStrategy} from './drag/inner_drag_strategy';
import {OuterDragStrategy} from './drag/outer_drag_strategy';
import {DragGestureHandler} from '../base/drag_gesture_handler';
import {
  getMaxMajorTicks,
  MIN_PX_PER_STEP,
  generateTicks,
  TickType,
} from './gridline_helper';
import {Size2D} from '../base/geom';
import {Panel} from './panel_container';
import {TimeScale} from '../base/time_scale';
import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import {TraceImpl} from '../core/trace_impl';
import {LONG, NUM} from '../trace_processor/query_result';
import {raf} from '../core/raf_scheduler';
import {getOrCreate} from '../base/utils';

const tracesData = new WeakMap<TraceImpl, OverviewDataLoader>();

export class OverviewTimelinePanel implements Panel {
  private static HANDLE_SIZE_PX = 5;
  readonly kind = 'panel';
  readonly selectable = false;
  private width = 0;
  private gesture?: DragGestureHandler;
  private timeScale?: TimeScale;
  private dragStrategy?: DragStrategy;
  private readonly boundOnMouseMove = this.onMouseMove.bind(this);
  private readonly overviewData: OverviewDataLoader;

  constructor(private trace: TraceImpl) {
    this.overviewData = getOrCreate(
      tracesData,
      trace,
      () => new OverviewDataLoader(trace),
    );
  }

  // Must explicitly type now; arguments types are no longer auto-inferred.
  // https://github.com/Microsoft/TypeScript/issues/1373
  onupdate({dom}: m.CVnodeDOM) {
    this.width = dom.getBoundingClientRect().width;
    const traceTime = this.trace.traceInfo;
    if (this.width > TRACK_SHELL_WIDTH) {
      const pxBounds = {left: TRACK_SHELL_WIDTH, right: this.width};
      const hpTraceTime = HighPrecisionTimeSpan.fromTime(
        traceTime.start,
        traceTime.end,
      );
      this.timeScale = new TimeScale(hpTraceTime, pxBounds);
      if (this.gesture === undefined) {
        this.gesture = new DragGestureHandler(
          dom as HTMLElement,
          this.onDrag.bind(this),
          this.onDragStart.bind(this),
          this.onDragEnd.bind(this),
        );
      }
    } else {
      this.timeScale = undefined;
    }
  }

  oncreate(vnode: m.CVnodeDOM) {
    this.onupdate(vnode);
    (vnode.dom as HTMLElement).addEventListener(
      'mousemove',
      this.boundOnMouseMove,
    );
  }

  onremove({dom}: m.CVnodeDOM) {
    if (this.gesture) {
      this.gesture[Symbol.dispose]();
      this.gesture = undefined;
    }
    (dom as HTMLElement).removeEventListener(
      'mousemove',
      this.boundOnMouseMove,
    );
  }

  render(): m.Children {
    return m('.overview-timeline', {
      oncreate: (vnode) => this.oncreate(vnode),
      onupdate: (vnode) => this.onupdate(vnode),
      onremove: (vnode) => this.onremove(vnode),
    });
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: Size2D) {
    if (this.width === undefined) return;
    if (this.timeScale === undefined) return;

    const headerHeight = 20;
    const tracksHeight = size.height - headerHeight;
    const traceContext = new TimeSpan(
      this.trace.traceInfo.start,
      this.trace.traceInfo.end,
    );

    if (size.width > TRACK_SHELL_WIDTH && traceContext.duration > 0n) {
      const maxMajorTicks = getMaxMajorTicks(this.width - TRACK_SHELL_WIDTH);
      const offset = this.trace.timeline.timestampOffset();
      const tickGen = generateTicks(traceContext, maxMajorTicks, offset);

      // Draw time labels
      ctx.font = '10px Roboto Condensed';
      ctx.fillStyle = '#999';
      for (const {type, time} of tickGen) {
        const xPos = Math.floor(this.timeScale.timeToPx(time));
        if (xPos <= 0) continue;
        if (xPos > this.width) break;
        if (type === TickType.MAJOR) {
          ctx.fillRect(xPos - 1, 0, 1, headerHeight - 5);
          const domainTime = this.trace.timeline.toDomainTime(time);
          renderTimestamp(ctx, domainTime, xPos + 5, 18, MIN_PX_PER_STEP);
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
          const xStart = Math.floor(this.timeScale.timeToPx(loads[i].start));
          const xEnd = Math.ceil(this.timeScale.timeToPx(loads[i].end));
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
    ctx.fillRect(0, size.height - 1, this.width, 1);

    // Draw semi-opaque rects that occlude the non-visible time range.
    const [vizStartPx, vizEndPx] = this.extractBounds(this.timeScale);

    ctx.fillStyle = OVERVIEW_TIMELINE_NON_VISIBLE_COLOR;
    ctx.fillRect(
      TRACK_SHELL_WIDTH - 1,
      headerHeight,
      vizStartPx - TRACK_SHELL_WIDTH,
      tracksHeight,
    );
    ctx.fillRect(vizEndPx, headerHeight, this.width - vizEndPx, tracksHeight);

    // Draw brushes.
    ctx.fillStyle = '#999';
    ctx.fillRect(vizStartPx - 1, headerHeight, 1, tracksHeight);
    ctx.fillRect(vizEndPx, headerHeight, 1, tracksHeight);

    const hbarWidth = OverviewTimelinePanel.HANDLE_SIZE_PX;
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
  }

  private onMouseMove(e: MouseEvent) {
    if (this.gesture === undefined || this.gesture.isDragging) {
      return;
    }
    (e.target as HTMLElement).style.cursor = this.chooseCursor(e.offsetX);
  }

  private chooseCursor(x: number) {
    if (this.timeScale === undefined) return 'default';
    const [startBound, endBound] = this.extractBounds(this.timeScale);
    if (
      OverviewTimelinePanel.inBorderRange(x, startBound) ||
      OverviewTimelinePanel.inBorderRange(x, endBound)
    ) {
      return 'ew-resize';
    } else if (x < TRACK_SHELL_WIDTH) {
      return 'default';
    } else if (x < startBound || endBound < x) {
      return 'crosshair';
    } else {
      return 'all-scroll';
    }
  }

  onDrag(x: number) {
    if (this.dragStrategy === undefined) return;
    this.dragStrategy.onDrag(x);
  }

  onDragStart(x: number) {
    if (this.timeScale === undefined) return;

    const cb = (vizTime: HighPrecisionTimeSpan) => {
      this.trace.timeline.updateVisibleTimeHP(vizTime);
      raf.scheduleRedraw();
    };
    const pixelBounds = this.extractBounds(this.timeScale);
    const timeScale = this.timeScale;
    if (
      OverviewTimelinePanel.inBorderRange(x, pixelBounds[0]) ||
      OverviewTimelinePanel.inBorderRange(x, pixelBounds[1])
    ) {
      this.dragStrategy = new BorderDragStrategy(timeScale, pixelBounds, cb);
    } else if (x < pixelBounds[0] || pixelBounds[1] < x) {
      this.dragStrategy = new OuterDragStrategy(timeScale, cb);
    } else {
      this.dragStrategy = new InnerDragStrategy(timeScale, pixelBounds, cb);
    }
    this.dragStrategy.onDragStart(x);
  }

  onDragEnd() {
    this.dragStrategy = undefined;
  }

  private extractBounds(timeScale: TimeScale): [number, number] {
    const vizTime = this.trace.timeline.visibleWindow;
    return [
      Math.floor(timeScale.hpTimeToPx(vizTime.start)),
      Math.ceil(timeScale.hpTimeToPx(vizTime.end)),
    ];
  }

  private static inBorderRange(a: number, b: number): boolean {
    return Math.abs(a - b) < this.HANDLE_SIZE_PX / 2;
  }
}

// Print a timestamp in the configured time format
function renderTimestamp(
  ctx: CanvasRenderingContext2D,
  time: time,
  x: number,
  y: number,
  minWidth: number,
): void {
  const fmt = timestampFormat();
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
    case TimestampFormat.Milliseoncds:
      ctx.fillText(Time.formatMilliseconds(time), x, y, minWidth);
      break;
    case TimestampFormat.Microseconds:
      ctx.fillText(Time.formatMicroseconds(time), x, y, minWidth);
      break;
    default:
      const z: never = fmt;
      throw new Error(`Invalid timestamp ${z}`);
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
          const schedResult = await this.trace.engine.query(
            `select cast(sum(dur) as float)/${stepSize} as load, cpu from sched ` +
              `where ts >= ${start} and ts < ${end} and utid != 0 ` +
              'group by cpu order by cpu',
          );
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
    const sliceResult = await this.trace.engine.query(`select
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
          group by bucket, upid`);

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
    raf.scheduleRedraw();
  }
}
