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

import {BigintMath as BIMath} from '../../base/bigint_math';
import {Monitor} from '../../base/monitor';
import {search, searchEq, searchSegment} from '../../base/binary_search';
import {assertTrue} from '../../base/logging';
import {duration, Time, time} from '../../base/time';
import {drawIncompleteSlice} from '../../base/canvas_utils';
import {cropText} from '../../base/string_utils';
import {Color} from '../../base/color';
import {ColorScheme} from '../../base/color_scheme';
import m from 'mithril';
import {colorForThread} from '../../components/colorizer';
import {CHUNKED_TASK_BACKGROUND_PRIORITY} from '../../components/tracks/feature_flags';
import {checkerboardExcept} from '../../components/checkerboard';
import {Point2D} from '../../base/geom';
import {HighPrecisionTime} from '../../base/high_precision_time';
import {TimeScale} from '../../base/time_scale';
import {TrackRenderer, SnapPoint} from '../../public/track';
import {LONG, NUM} from '../../trace_processor/query_result';
import {TrackMouseEvent, TrackRenderContext} from '../../public/track';
import {TrackEventDetails} from '../../public/selection';
import {SchedSliceDetailsPanel} from './sched_details_tab';
import {Trace} from '../../public/trace';
import {ThreadMap} from '../dev.perfetto.Thread/threads';
import {SourceDataset} from '../../trace_processor/dataset';
import {deferChunkedTask} from '../../base/chunked_task';
import {RECT_PATTERN_HATCHED} from '../../base/renderer';
import {
  CancellationSignal,
  QuerySlot,
  QUERY_CANCELLED,
  SerialTaskQueue,
} from '../../base/query_slot';
import {createVirtualTable} from '../../trace_processor/sql_utils';
import {BufferedBounds} from '../../components/tracks/buffered_bounds';

export interface Data {
  // Slices are stored in a columnar fashion. All fields have the same length.
  start: time;
  end: time;
  resolution: duration;
  length: number;
  counts: Float64Array;
  ids: Float64Array;
  startQs: BigInt64Array;
  endQs: BigInt64Array;
  tses: BigInt64Array;
  durs: BigInt64Array;
  utids: Uint32Array;
  pids: BigInt64Array;
  flags: Uint8Array;
  lastRowId: number;
  // Cached color schemes to avoid lookups in render hot path
  colorSchemes: ColorScheme[];
  // Relative timestamps for fast rendering (avoids BigInt conversion in hot path)
  // All times are relative to data.start (in nanoseconds as floats)
  startRelNs: Float64Array;
  endRelNs: Float64Array;
}

interface MipmapTable extends AsyncDisposable {
  tableName: string;
  lastRowId: number;
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
  data: Data,
  threads: ThreadMap,
): CpuSliceHover | undefined {
  if (pos === undefined) return undefined;

  const {x, y} = pos;
  if (y < MARGIN_TOP || y > MARGIN_TOP + RECT_HEIGHT) return undefined;

  const t = timescale.pxToHpTime(x);
  for (let i = 0; i < data.startQs.length; i++) {
    const tStart = Time.fromRaw(data.startQs[i]);
    const tEnd = Time.fromRaw(data.endQs[i]);
    if (t.containedWithin(tStart, tEnd)) {
      const utid = data.utids[i];
      const count = data.counts[i];
      const pid = threads.get(utid)?.pid;
      return {utid, count, pid};
    }
  }
  return undefined;
}

export class CpuSliceTrack implements TrackRenderer {
  private hover?: CpuSliceHover;

  // QuerySlot infrastructure
  private readonly queue = new SerialTaskQueue();
  private readonly tableSlot = new QuerySlot<MipmapTable>(this.queue);
  private readonly dataSlot = new QuerySlot<Data>(this.queue);
  private data?: Data;

  // Buffered bounds tracking
  private readonly bufferedBounds = new BufferedBounds();

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

  private async createMipmapTable(): Promise<MipmapTable> {
    const table = await createVirtualTable({
      engine: this.trace.engine,
      using: `__intrinsic_slice_mipmap((
        select
          id,
          ts,
          iif(dur = -1, lead(ts, 1, trace_end()) over (order by ts) - ts, dur) as dur,
          0 as depth
        from sched
        where ucpu = ${this.ucpu} and
          not utid in (select utid from thread where is_idle)
      ))`,
    });

    const it = await this.trace.engine.query(`
      select coalesce(max(id), -1) as lastRowId
      from sched
      where ucpu = ${this.ucpu} and
        not utid in (select utid from thread where is_idle)
    `);
    const lastRowId = it.firstRow({lastRowId: NUM}).lastRowId;

    return {
      tableName: table.name,
      lastRowId,
      [Symbol.asyncDispose]: async () => {
        await table[Symbol.asyncDispose]();
      },
    };
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

  private async fetchData(
    tableName: string,
    lastRowId: number,
    start: time,
    end: time,
    resolution: duration,
    signal: CancellationSignal,
  ): Promise<Data> {
    assertTrue(BIMath.popcount(resolution) === 1, `${resolution} not pow of 2`);

    const queryRes = await this.trace.engine.query(`
      select
        (z.ts / ${resolution}) * ${resolution} as tsQ,
        (((z.ts + z.dur) / ${resolution}) + 1) * ${resolution} as tsEndQ,
        z.count,
        s.ts,
        s.dur,
        s.utid,
        s.id,
        s.dur = -1 as isIncomplete,
        ifnull(s.priority < 100, 0) as isRealtime
      from ${tableName}(${start}, ${end}, ${resolution}) z
      cross join sched s using (id)
    `);

    if (signal.isCancelled) throw QUERY_CANCELLED;

    const priority = CHUNKED_TASK_BACKGROUND_PRIORITY.get()
      ? 'background'
      : undefined;
    const task = await deferChunkedTask({priority});

    const numRows = queryRes.numRows();
    const slices: Data = {
      start,
      end,
      resolution,
      length: numRows,
      lastRowId,
      counts: new Float64Array(numRows),
      ids: new Float64Array(numRows),
      startQs: new BigInt64Array(numRows),
      endQs: new BigInt64Array(numRows),
      tses: new BigInt64Array(numRows),
      durs: new BigInt64Array(numRows),
      utids: new Uint32Array(numRows),
      pids: new BigInt64Array(numRows),
      flags: new Uint8Array(numRows),
      colorSchemes: new Array(numRows),
      // Relative timestamps for fast rendering (relative to data.start)
      startRelNs: new Float64Array(numRows),
      endRelNs: new Float64Array(numRows),
    };

    const it = queryRes.iter({
      count: NUM,
      tsQ: LONG,
      tsEndQ: LONG,
      ts: LONG,
      dur: LONG,
      utid: NUM,
      id: NUM,
      isIncomplete: NUM,
      isRealtime: NUM,
    });
    for (let row = 0; it.valid(); it.next(), row++) {
      if (signal.isCancelled) throw QUERY_CANCELLED;
      if (row % 50 === 0 && task.shouldYield()) {
        await task.yield();
      }

      slices.counts[row] = it.count;
      slices.startQs[row] = it.tsQ;
      slices.endQs[row] = it.tsEndQ;
      slices.tses[row] = it.ts;
      slices.durs[row] = it.dur;
      slices.utids[row] = it.utid;
      slices.pids[row] = this.threads.get(it.utid)?.pid ?? -1n;
      slices.ids[row] = it.id;

      // Store relative timestamps as floats for fast rendering
      slices.startRelNs[row] = Number(it.tsQ - start);
      slices.endRelNs[row] = Number(it.tsEndQ - start);

      slices.flags[row] = 0;
      if (it.isIncomplete) {
        slices.flags[row] |= CPU_SLICE_FLAGS_INCOMPLETE;
      }
      if (it.isRealtime) {
        slices.flags[row] |= CPU_SLICE_FLAGS_REALTIME;
      }

      // Cache color scheme to avoid lookups in render hot path
      const threadInfo = this.threads.get(it.utid);
      slices.colorSchemes[row] = colorForThread(threadInfo);
    }
    return slices;
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
    const {ctx, size, timescale, visibleWindow, resolution, renderer} =
      trackCtx;

    // Get the mipmap table (created once)
    const tableResult = this.tableSlot.use({
      key: {ucpu: this.ucpu},
      queryFn: () => this.createMipmapTable(),
    });

    const table = tableResult.data;
    if (table === undefined) return; // Table not ready yet

    // Calculate buffered bounds - expand to double the visible window
    // Only refetch when visible window exceeds loaded bounds
    const visibleSpan = visibleWindow.toTimeSpan();
    const bounds = this.bufferedBounds.update(
      visibleSpan,
      resolution,
      this.data !== undefined,
    );

    // Fetch data using the current buffered bounds
    const dataResult = this.dataSlot.use({
      key: {
        start: bounds.start,
        end: bounds.end,
        resolution: bounds.resolution,
      },
      queryFn: async (signal) => {
        const result = await this.fetchData(
          table.tableName,
          table.lastRowId,
          bounds.start,
          bounds.end,
          bounds.resolution,
          signal,
        );
        this.trace.raf.scheduleCanvasRedraw();
        return result;
      },
      retainOn: ['start', 'end', 'resolution'],
    });

    if (dataResult.data !== undefined) {
      this.data = dataResult.data;
    }

    const data = this.data;
    if (data === undefined) return; // Can't possibly draw anything.

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
      ctx,
      this.getHeight(),
      0,
      size.width,
      timescale.timeToPx(data.start),
      timescale.timeToPx(data.end),
    );

    assertTrue(data.startQs.length === data.endQs.length);
    assertTrue(data.startQs.length === data.utids.length);

    const visWindowEndPx = size.width;

    ctx.textAlign = 'center';
    ctx.font = '12px Roboto Condensed';
    const charWidth = ctx.measureText('dbpqaouk').width / 8;

    const timespan = visibleWindow.toTimeSpan();

    const startTime = timespan.start;
    const endTime = timespan.end;

    // Pre-compute conversion factors for fast timestamp-to-pixel conversion.
    // Formula: px = relativeNs * pxPerNs + baseOffsetPx
    const pxPerNs = timescale.durationToPx(1n);
    const baseOffsetPx = timescale.timeToPx(data.start);

    const rawStartIdx = data.endQs.findIndex((end) => end >= startTime);
    const startIdx = rawStartIdx === -1 ? 0 : rawStartIdx;

    const [, rawEndIdx] = searchSegment(data.startQs, endTime);
    const endIdx = rawEndIdx === -1 ? data.startQs.length : rawEndIdx;

    const timeline = this.trace.timeline;
    const hoveredUtid = timeline.hoveredUtid;
    const hoveredPid = timeline.hoveredPid;

    // Collect text labels to render in a second pass (allows batching rects)
    const textLabels: Array<{
      title: string;
      subTitle: string;
      textColor: Color;
      rectXCenter: number;
    }> = [];

    // Collect incomplete slices to render after flushing
    const incompleteSlices: Array<{
      rectStart: number;
      rectWidth: number;
      color: Color;
    }> = [];

    // First pass: draw all rects (batched via renderer)
    for (let i = startIdx; i < endIdx; i++) {
      const utid = data.utids[i];
      const pid = data.pids[i];
      const colorScheme = data.colorSchemes[i];
      const flags = data.flags[i];
      const id = data.ids[i];

      // Use pre-computed relative timestamps for fast pixel conversion
      const rectStart = data.startRelNs[i] * pxPerNs + baseOffsetPx;

      // If the last slice is incomplete, it should end with the end of the
      // window, else it might spill over the window and the end would not be
      // visible as a zigzag line.
      const isIncomplete =
        id === data.lastRowId && flags & CPU_SLICE_FLAGS_INCOMPLETE;
      const rectEnd = Boolean(isIncomplete)
        ? visWindowEndPx
        : data.endRelNs[i] * pxPerNs + baseOffsetPx;
      const rectWidth = Math.max(1, rectEnd - rectStart);
      const isHovering = hoveredUtid !== undefined;
      const isThreadHovered = hoveredUtid === utid;
      const isProcessHovered = hoveredPid === pid;

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

      if (flags & CPU_SLICE_FLAGS_INCOMPLETE) {
        // Defer incomplete slices to render after flush
        incompleteSlices.push({rectStart, rectWidth, color});
      } else {
        renderer.drawRect(
          rectStart,
          MARGIN_TOP,
          rectStart + rectWidth,
          MARGIN_TOP + RECT_HEIGHT,
          color,
        );
      }

      // Stylize real-time threads. We don't do it when zoomed out as the
      // fillRect is expensive.
      if (flags & CPU_SLICE_FLAGS_REALTIME) {
        renderer.drawRect(
          rectStart,
          MARGIN_TOP,
          rectStart + rectWidth,
          MARGIN_TOP + RECT_HEIGHT,
          color,
          RECT_PATTERN_HATCHED,
        );
      }

      // Don't render text when we have less than 5px to play with.
      if (rectWidth < 5) continue;

      // TODO: consider de-duplicating this code with the copied one from
      // chrome_slices/frontend.ts.
      let title = `[utid:${utid}]`;
      let subTitle = '';
      const threadInfo = this.threads.get(utid);
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

      if (flags & CPU_SLICE_FLAGS_REALTIME) {
        subTitle = subTitle + ' (RT)';
      }

      const right = Math.min(visWindowEndPx, rectEnd);
      const left = Math.max(rectStart, 0);
      const visibleWidth = Math.max(right - left, 1);

      textLabels.push({
        title: cropText(title, charWidth, visibleWidth),
        subTitle: cropText(subTitle, charWidth, visibleWidth),
        textColor,
        rectXCenter: left + visibleWidth / 2,
      });
    }

    // Flush once after all rects are batched
    renderer.flush();

    // Draw incomplete slices (requires direct canvas access)
    for (const {rectStart, rectWidth, color} of incompleteSlices) {
      ctx.fillStyle = color.cssString;
      drawIncompleteSlice(
        ctx,
        rectStart,
        MARGIN_TOP,
        rectWidth,
        RECT_HEIGHT,
        color,
      );
    }

    // Second pass: draw all text labels
    for (const {title, subTitle, textColor, rectXCenter} of textLabels) {
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
        const [startIndex, endIndex] = searchEq(data.ids, selection.eventId);
        if (startIndex !== endIndex) {
          const color = data.colorSchemes[startIndex];
          const rectStart =
            data.startRelNs[startIndex] * pxPerNs + baseOffsetPx;
          const rectEnd = data.endRelNs[startIndex] * pxPerNs + baseOffsetPx;
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
    const data = this.data;
    if (data === undefined) return;
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
    const data = this.data;
    if (data === undefined) return false;
    const time = timescale.pxToHpTime(x);
    const index = search(data.startQs, time.toTime());
    const id = index === -1 ? undefined : data.ids[index];
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
    const data = this.data;
    if (data === undefined) {
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
    for (let i = 0; i < data.startQs.length; i++) {
      // Check start boundary
      checkBoundary(Time.fromRaw(data.tses[i]));

      // Check end boundary
      checkBoundary(Time.fromRaw(data.tses[i] + data.durs[i]));
    }

    return closestSnap;
  }

  detailsPanel() {
    return new SchedSliceDetailsPanel(this.trace, this.threads);
  }
}
