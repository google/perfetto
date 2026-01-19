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

import {Monitor} from '../../base/monitor';
import {searchSorted} from '../../base/binary_search';
import {assertExists} from '../../base/logging';
import {Time, time} from '../../base/time';
import {drawIncompleteSlice} from '../../base/canvas_utils';
import {cropText} from '../../base/string_utils';
import {Color} from '../../base/color';
import m from 'mithril';
import {colorForThread} from '../../components/colorizer';
import {checkerboardExcept} from '../../components/checkerboard';
import {CacheKey} from '../../components/tracks/timeline_cache';
import {TrackRenderPipeline} from '../../components/tracks/track_render_pipeline';
import {Point2D} from '../../base/geom';
import {HighPrecisionTime} from '../../base/high_precision_time';
import {TimeScale} from '../../base/time_scale';
import {TrackRenderer, SnapPoint, TrackUpdateContext} from '../../public/track';
import {LONG, NUM, Row} from '../../trace_processor/query_result';
import {uuidv4Sql} from '../../base/uuid';
import {TrackMouseEvent, TrackRenderContext} from '../../public/track';
import {TrackEventDetails} from '../../public/selection';
import {SchedSliceDetailsPanel} from './sched_details_tab';
import {Trace} from '../../public/trace';
import {ThreadMap} from '../dev.perfetto.Thread/threads';
import {SourceDataset} from '../../trace_processor/dataset';

// Row spec for the CPU slice mipmap query.
const CPU_SLICE_ROW = {
  count: NUM,
  tsQ: LONG,
  tsEndQ: LONG,
  ts: LONG,
  dur: LONG,
  utid: NUM,
  id: NUM,
  isIncomplete: NUM,
  isRealtime: NUM,
};
type CpuSliceRow = typeof CPU_SLICE_ROW;

// Entry stored in the pipeline buffer.
interface CpuSliceEntry {
  count: number;
  id: number;
  startQ: time;
  endQ: time;
  ts: time;
  dur: bigint;
  utid: number;
  flags: number;
}

const MARGIN_TOP = 3;
const RECT_HEIGHT = 24;
const TRACK_HEIGHT = MARGIN_TOP * 2 + RECT_HEIGHT;

const CPU_SLICE_FLAGS_INCOMPLETE = 1;
const CPU_SLICE_FLAGS_REALTIME = 2;

interface CpuSliceHover {
  utid: number;
  count: number;
  pid?: bigint;
}

function computeHover(
  pos: Point2D | undefined,
  timescale: TimeScale,
  data: CpuSliceEntry[] | undefined,
  threads: ThreadMap,
): CpuSliceHover | undefined {
  if (pos === undefined) return undefined;
  if (data === undefined || data.length === 0) return undefined;

  const {x, y} = pos;
  if (y < MARGIN_TOP || y > MARGIN_TOP + RECT_HEIGHT) return undefined;

  const t = timescale.pxToHpTime(x);
  for (const entry of data) {
    if (t.containedWithin(entry.startQ, entry.endQ)) {
      const pid = threads.get(entry.utid)?.pid;
      return {utid: entry.utid, count: entry.count, pid};
    }
  }
  return undefined;
}

export class CpuSliceTrack implements TrackRenderer {
  private hover?: CpuSliceHover;

  private lastRowId = -1;
  private trackUuid = uuidv4Sql();
  private cacheKey = CacheKey.zero();

  // Handles data loading with viewport caching, double-buffering, cooperative
  // multitasking, and abort detection when the viewport changes.
  private pipeline?: TrackRenderPipeline<
    Row & CpuSliceRow,
    CpuSliceEntry,
    {lastRowId: number}
  >;

  // Monitor for local hover state (triggers DOM redraw for tooltip).
  private readonly hoverMonitor = new Monitor([
    () => this.hover?.utid,
    () => this.hover?.count,
  ]);

  readonly rootTableName = 'sched_slice';

  constructor(
    private readonly trace: Trace,
    private readonly uri: string,
    private readonly ucpu: number,
    private readonly threads: ThreadMap,
  ) {}

  async onCreate() {
    await this.trace.engine.query(`
      create virtual table cpu_slice_${this.trackUuid}
      using __intrinsic_slice_mipmap((
        select
          id,
          ts,
          iif(dur = -1, lead(ts, 1, trace_end()) over (order by ts) - ts, dur) as dur,
          0 as depth
        from sched
        where ucpu = ${this.ucpu} and
          not utid in (select utid from thread where is_idle)
      ));
    `);
    const it = await this.trace.engine.query(`
      select coalesce(max(id), -1) as lastRowId
      from sched
      where ucpu = ${this.ucpu} and
        not utid in (select utid from thread where is_idle)
    `);
    this.lastRowId = it.firstRow({lastRowId: NUM}).lastRowId;

    // Initialize the pipeline.
    this.pipeline = new TrackRenderPipeline(
      this.trace,
      (_rawSql: string, key: CacheKey) => `
        select
          (z.ts / ${key.bucketSize}) * ${key.bucketSize} as tsQ,
          (((z.ts + z.dur) / ${key.bucketSize}) + 1) * ${key.bucketSize} as tsEndQ,
          z.count,
          s.ts,
          s.dur,
          s.utid,
          s.id,
          s.dur = -1 as isIncomplete,
          ifnull(s.priority < 100, 0) as isRealtime
        from cpu_slice_${this.trackUuid}(${key.start}, ${key.end}, ${key.bucketSize}) z
        cross join sched s using (id)
      `,
      () => ({lastRowId: this.lastRowId}),
      (row, _state) => {
        let flags = 0;
        if (row.isIncomplete) flags |= CPU_SLICE_FLAGS_INCOMPLETE;
        if (row.isRealtime) flags |= CPU_SLICE_FLAGS_REALTIME;
        return {
          count: row.count,
          id: row.id,
          startQ: Time.fromRaw(row.tsQ),
          endQ: Time.fromRaw(row.tsEndQ),
          ts: Time.fromRaw(row.ts),
          dur: row.dur,
          utid: row.utid,
          flags,
        };
      },
    );
  }

  getDataset() {
    return new SourceDataset({
      // TODO(stevegolton): Once we allow datasets to have more than one filter,
      // move this where clause to a dataset filter and change this src to
      // 'sched'.
      src: `select id, ts, dur, ucpu, utid
            from sched
            where not utid in (select utid from thread where is_idle)`,
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        ucpu: NUM,
        utid: NUM,
      },
      filter: {
        col: 'ucpu',
        eq: this.ucpu,
      },
    });
  }

  async onUpdate(ctx: TrackUpdateContext): Promise<void> {
    if (this.pipeline === undefined) return;

    const result = await this.pipeline.onUpdate('', CPU_SLICE_ROW, ctx);
    if (result === 'updated') {
      this.cacheKey = this.pipeline.getCacheKey();
    }
  }

  async onDestroy() {
    await this.trace.engine.tryQuery(
      `drop table if exists cpu_slice_${this.trackUuid}`,
    );
  }

  getHeight(): number {
    return TRACK_HEIGHT;
  }

  renderTooltip(): m.Children {
    if (this.hover === undefined) {
      return undefined;
    }

    const hoveredThread = this.threads.get(this.hover.utid);
    if (!hoveredThread) {
      return undefined;
    }

    const tidText = `T: ${hoveredThread.threadName} [${hoveredThread.tid}]`;

    const count = this.hover.count;
    const countDiv = count > 1 && m('div', `and ${count - 1} other events`);
    if (hoveredThread.pid !== undefined) {
      const pidText = `P: ${hoveredThread.procName} [${hoveredThread.pid}]`;
      return m('.tooltip', [m('div', pidText), m('div', tidText), countDiv]);
    } else {
      return m('.tooltip', tidText, countDiv);
    }
  }

  render(trackCtx: TrackRenderContext): void {
    const {ctx, size, timescale} = trackCtx;

    // TODO: fonts and colors should come from the CSS and not hardcoded here.
    const data = this.pipeline?.getActiveBuffer() ?? [];

    if (data.length === 0) return; // Can't possibly draw anything.

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
      ctx,
      this.getHeight(),
      0,
      size.width,
      timescale.timeToPx(this.cacheKey.start),
      timescale.timeToPx(this.cacheKey.end),
    );

    this.renderSlices(trackCtx, data);
  }

  renderSlices(
    {ctx, timescale, size, visibleWindow}: TrackRenderContext,
    data: CpuSliceEntry[],
  ): void {
    const visWindowEndPx = size.width;

    ctx.textAlign = 'center';
    ctx.font = '12px Roboto Condensed';
    const charWidth = ctx.measureText('dbpqaouk').width / 8;

    const timespan = visibleWindow.toTimeSpan();
    const endTime = timespan.end;

    // Find the end of visible range - binary search for last slice whose start <= endTime.
    // This allows us to skip iterating over slices that are completely after the visible window.
    const lastVisibleIdx = searchSorted(data, endTime, (e) => e.startQ);
    const endIdx = lastVisibleIdx === -1 ? 0 : lastVisibleIdx + 1;

    for (let i = 0; i < endIdx; i++) {
      const entry = data[i];
      const tStart = entry.startQ;
      let tEnd = entry.endQ;
      const utid = entry.utid;

      // If the last slice is incomplete, it should end with the end of the
      // window, else it might spill over the window and the end would not be
      // visible as a zigzag line.
      if (
        entry.id === this.lastRowId &&
        entry.flags & CPU_SLICE_FLAGS_INCOMPLETE
      ) {
        tEnd = endTime;
      }
      const rectStart = timescale.timeToPx(tStart);
      const rectEnd = timescale.timeToPx(tEnd);
      const rectWidth = Math.max(1, rectEnd - rectStart);

      const threadInfo = this.threads.get(utid);
      // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
      const pid = threadInfo && threadInfo.pid ? threadInfo.pid : -1;

      const isHovering = this.trace.timeline.hoveredUtid !== undefined;
      const isThreadHovered = this.trace.timeline.hoveredUtid === utid;
      const isProcessHovered = this.trace.timeline.hoveredPid === pid;
      const colorScheme = colorForThread(threadInfo);
      let color: Color;
      let textColor: Color;
      if (isHovering && !isThreadHovered) {
        if (!isProcessHovered) {
          color = colorScheme.disabled;
          textColor = colorScheme.textDisabled;
        } else {
          color = colorScheme.variant;
          textColor = colorScheme.textVariant;
        }
      } else {
        color = colorScheme.base;
        textColor = colorScheme.textBase;
      }
      ctx.fillStyle = color.cssString;

      if (entry.flags & CPU_SLICE_FLAGS_INCOMPLETE) {
        drawIncompleteSlice(
          ctx,
          rectStart,
          MARGIN_TOP,
          rectWidth,
          RECT_HEIGHT,
          color,
        );
      } else {
        ctx.fillRect(rectStart, MARGIN_TOP, rectWidth, RECT_HEIGHT);
      }

      // Don't render text when we have less than 5px to play with.
      if (rectWidth < 5) continue;

      // Stylize real-time threads. We don't do it when zoomed out as the
      // fillRect is expensive.
      if (entry.flags & CPU_SLICE_FLAGS_REALTIME) {
        ctx.fillStyle = getHatchedPattern(ctx);
        ctx.fillRect(rectStart, MARGIN_TOP, rectWidth, RECT_HEIGHT);
      }

      // TODO: consider de-duplicating this code with the copied one from
      // chrome_slices/frontend.ts.
      let title = `[utid:${utid}]`;
      let subTitle = '';
      if (threadInfo) {
        if (threadInfo.pid !== undefined && threadInfo.pid !== 0n) {
          let procName = threadInfo.procName ?? '';
          if (procName.startsWith('/')) {
            // Remove folder paths from name
            procName = procName.substring(procName.lastIndexOf('/') + 1);
          }
          title = `${procName} [${threadInfo.pid}]`;
          subTitle = `${threadInfo.threadName} [${threadInfo.tid}]`;
        } else {
          title = `${threadInfo.threadName} [${threadInfo.tid}]`;
        }
      }

      if (entry.flags & CPU_SLICE_FLAGS_REALTIME) {
        subTitle = subTitle + ' (RT)';
      }

      const right = Math.min(visWindowEndPx, rectEnd);
      const left = Math.max(rectStart, 0);
      const visibleWidth = Math.max(right - left, 1);
      title = cropText(title, charWidth, visibleWidth);
      subTitle = cropText(subTitle, charWidth, visibleWidth);
      const rectXCenter = left + visibleWidth / 2;
      ctx.fillStyle = textColor.cssString;
      ctx.font = '12px Roboto Condensed';
      ctx.fillText(title, rectXCenter, MARGIN_TOP + RECT_HEIGHT / 2 - 1);
      ctx.fillStyle = textColor.setAlpha(0.6).cssString;
      ctx.font = '10px Roboto Condensed';
      ctx.fillText(subTitle, rectXCenter, MARGIN_TOP + RECT_HEIGHT / 2 + 9);
    }

    const selection = this.trace.selection.selection;
    if (selection.kind === 'track_event') {
      if (selection.trackUri === this.uri) {
        // Find selected entry
        const selectedEntry = data.find((e) => e.id === selection.eventId);
        if (selectedEntry !== undefined) {
          const tStart = selectedEntry.startQ;
          const tEnd = selectedEntry.endQ;
          const utid = selectedEntry.utid;
          const color = colorForThread(this.threads.get(utid));
          const rectStart = timescale.timeToPx(tStart);
          const rectEnd = timescale.timeToPx(tEnd);
          const rectWidth = Math.max(1, rectEnd - rectStart);

          // Draw a rectangle around the slice that is currently selected.
          ctx.strokeStyle = color.base.setHSL({l: 30}).cssString;
          ctx.beginPath();
          ctx.lineWidth = 3;
          ctx.strokeRect(
            rectStart,
            MARGIN_TOP - 1.5,
            rectWidth,
            RECT_HEIGHT + 3,
          );
          ctx.closePath();
        }
      }
    }
  }

  onMouseMove({x, y, timescale}: TrackMouseEvent) {
    const data = this.pipeline?.getActiveBuffer();
    this.hover = computeHover({x, y}, timescale, data, this.threads);
    if (this.hoverMonitor.ifStateChanged()) {
      this.trace.timeline.hoveredUtid = this.hover?.utid;
      this.trace.timeline.hoveredPid = this.hover?.pid;
      this.trace.raf.scheduleFullRedraw();
    }
  }

  onMouseOut() {
    this.hover = undefined;
    if (this.hoverMonitor.ifStateChanged()) {
      this.trace.timeline.hoveredUtid = undefined;
      this.trace.timeline.hoveredPid = undefined;
      this.trace.raf.scheduleFullRedraw();
    }
  }

  onMouseClick({x, timescale}: TrackMouseEvent) {
    const data = this.pipeline?.getActiveBuffer();
    if (data === undefined || data.length === 0) return false;
    const targetTime = timescale.pxToHpTime(x).toTime();
    const idx = searchSorted(data, targetTime, (e) => e.startQ);
    const id = idx === -1 ? undefined : data[idx].id;
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    if (!id || this.hover === undefined) return false;

    this.trace.selection.selectTrackEvent(this.uri, id);
    return true;
  }

  async getSelectionDetails?(
    eventId: number,
  ): Promise<TrackEventDetails | undefined> {
    const dataset = this.getDataset();
    const result = await this.trace.engine.query(`
      SELECT
        ts,
        dur
      FROM (${dataset.query()})
      WHERE id = ${eventId}
    `);

    const firstRow = result.maybeFirstRow({
      ts: LONG,
      dur: LONG,
    });

    if (firstRow) {
      return {
        ts: Time.fromRaw(firstRow.ts),
        dur: firstRow.dur,
      };
    } else {
      return undefined;
    }
  }

  getSnapPoint(
    targetTime: time,
    thresholdPx: number,
    timescale: TimeScale,
  ): SnapPoint | undefined {
    const data = this.pipeline?.getActiveBuffer();
    if (data === undefined || data.length === 0) {
      return undefined;
    }

    // Convert pixel threshold to time duration (in nanoseconds as number)
    const thresholdNs = timescale.pxToDuration(thresholdPx);

    // Use HighPrecisionTime to handle time arithmetic with fractional nanoseconds
    const hpTargetTime = new HighPrecisionTime(targetTime);
    const hpSearchStart = hpTargetTime.addNumber(-thresholdNs);
    const hpSearchEnd = hpTargetTime.addNumber(thresholdNs);

    // Convert back to time for comparisons
    const searchStart = hpSearchStart.toTime();
    const searchEnd = hpSearchEnd.toTime();

    let closestSnap: SnapPoint | undefined = undefined;
    let closestDistNs = thresholdNs;

    // Helper function to check a boundary
    const checkBoundary = (boundaryTime: time) => {
      // Skip if outside search window
      if (boundaryTime < searchStart || boundaryTime > searchEnd) {
        return;
      }

      // Calculate distance using HighPrecisionTime for accuracy
      const hpBoundary = new HighPrecisionTime(boundaryTime);
      const distNs = Math.abs(hpTargetTime.sub(hpBoundary).toNumber());

      if (distNs < closestDistNs) {
        closestSnap = {
          time: boundaryTime,
        };
        closestDistNs = distNs;
      }
    };

    // Iterate through all slices in the cached data
    for (const entry of data) {
      // Check start boundary
      checkBoundary(entry.ts);

      // Check end boundary
      checkBoundary(Time.add(entry.ts, entry.dur));
    }

    return closestSnap;
  }

  detailsPanel() {
    return new SchedSliceDetailsPanel(this.trace, this.threads);
  }
}

// Creates a diagonal hatched pattern to be used for distinguishing slices with
// real-time priorities. The pattern is created once as an offscreen canvas and
// is kept cached inside the Context2D of the main canvas, without making
// assumptions on the lifetime of the main canvas.
function getHatchedPattern(mainCtx: CanvasRenderingContext2D): CanvasPattern {
  const mctx = mainCtx as CanvasRenderingContext2D & {
    sliceHatchedPattern?: CanvasPattern;
  };
  if (mctx.sliceHatchedPattern !== undefined) return mctx.sliceHatchedPattern;
  const canvas = document.createElement('canvas');
  const SIZE = 8;
  canvas.width = canvas.height = SIZE;
  const ctx = assertExists(canvas.getContext('2d'));
  ctx.strokeStyle = 'rgba(255,255,255,0.3)';
  ctx.beginPath();
  ctx.lineWidth = 1;
  ctx.moveTo(0, SIZE);
  ctx.lineTo(SIZE, 0);
  ctx.stroke();
  mctx.sliceHatchedPattern = assertExists(mctx.createPattern(canvas, 'repeat'));
  return mctx.sliceHatchedPattern;
}
