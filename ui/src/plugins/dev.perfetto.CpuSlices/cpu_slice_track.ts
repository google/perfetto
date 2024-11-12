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
import {search, searchEq, searchSegment} from '../../base/binary_search';
import {assertExists, assertTrue} from '../../base/logging';
import {Duration, duration, Time, time} from '../../base/time';
import {
  drawDoubleHeadedArrow,
  drawIncompleteSlice,
  drawTrackHoverTooltip,
} from '../../base/canvas_utils';
import {cropText} from '../../base/string_utils';
import {Color} from '../../public/color';
import {colorForThread} from '../../public/lib/colorizer';
import {TrackData} from '../../common/track_data';
import {TimelineFetcher} from '../../common/track_helper';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {Point2D} from '../../base/geom';
import {Track} from '../../public/track';
import {LONG, NUM} from '../../trace_processor/query_result';
import {uuidv4Sql} from '../../base/uuid';
import {TrackMouseEvent, TrackRenderContext} from '../../public/track';
import {TrackEventDetails} from '../../public/selection';
import {asSchedSqlId} from '../../trace_processor/sql_utils/core_types';
import {
  getSched,
  getSchedWakeupInfo,
} from '../../trace_processor/sql_utils/sched';
import {SchedSliceDetailsPanel} from './sched_details_tab';
import {Trace} from '../../public/trace';
import {exists} from '../../base/utils';
import {ThreadMap} from '../dev.perfetto.Thread/threads';

export interface Data extends TrackData {
  // Slices are stored in a columnar fashion. All fields have the same length.
  ids: Float64Array;
  startQs: BigInt64Array;
  endQs: BigInt64Array;
  utids: Uint32Array;
  flags: Uint8Array;
  lastRowId: number;
}

const MARGIN_TOP = 3;
const RECT_HEIGHT = 24;
const TRACK_HEIGHT = MARGIN_TOP * 2 + RECT_HEIGHT;

const CPU_SLICE_FLAGS_INCOMPLETE = 1;
const CPU_SLICE_FLAGS_REALTIME = 2;

export class CpuSliceTrack implements Track {
  private mousePos?: Point2D;
  private utidHoveredInThisTrack?: number;
  private fetcher = new TimelineFetcher<Data>(this.onBoundsChange.bind(this));

  private lastRowId = -1;
  private trackUuid = uuidv4Sql();

  constructor(
    private readonly trace: Trace,
    private readonly uri: string,
    private readonly cpu: number,
    private readonly threads: ThreadMap,
  ) {}

  async onCreate() {
    await this.trace.engine.query(`
      create virtual table cpu_slice_${this.trackUuid}
      using __intrinsic_slice_mipmap((
        select
          id,
          ts,
          iif(dur = -1, lead(ts, 1, trace_end()) over (order by ts) - ts, dur),
          0 as depth
        from sched
        where cpu = ${this.cpu} and utid != 0
      ));
    `);
    const it = await this.trace.engine.query(`
      select coalesce(max(id), -1) as lastRowId
      from sched
      where cpu = ${this.cpu} and utid != 0
    `);
    this.lastRowId = it.firstRow({lastRowId: NUM}).lastRowId;
  }

  async onUpdate({
    visibleWindow,
    resolution,
  }: TrackRenderContext): Promise<void> {
    await this.fetcher.requestData(visibleWindow.toTimeSpan(), resolution);
  }

  async onBoundsChange(
    start: time,
    end: time,
    resolution: duration,
  ): Promise<Data> {
    assertTrue(BIMath.popcount(resolution) === 1, `${resolution} not pow of 2`);

    const queryRes = await this.trace.engine.query(`
      select
        (z.ts / ${resolution}) * ${resolution} as tsQ,
        (((z.ts + z.dur) / ${resolution}) + 1) * ${resolution} as tsEndQ,
        s.utid,
        s.id,
        s.dur = -1 as isIncomplete,
        ifnull(s.priority < 100, 0) as isRealtime
      from cpu_slice_${this.trackUuid}(${start}, ${end}, ${resolution}) z
      cross join sched s using (id)
    `);

    const numRows = queryRes.numRows();
    const slices: Data = {
      start,
      end,
      resolution,
      length: numRows,
      lastRowId: this.lastRowId,
      ids: new Float64Array(numRows),
      startQs: new BigInt64Array(numRows),
      endQs: new BigInt64Array(numRows),
      utids: new Uint32Array(numRows),
      flags: new Uint8Array(numRows),
    };

    const it = queryRes.iter({
      tsQ: LONG,
      tsEndQ: LONG,
      utid: NUM,
      id: NUM,
      isIncomplete: NUM,
      isRealtime: NUM,
    });
    for (let row = 0; it.valid(); it.next(), row++) {
      slices.startQs[row] = it.tsQ;
      slices.endQs[row] = it.tsEndQ;
      slices.utids[row] = it.utid;
      slices.ids[row] = it.id;

      slices.flags[row] = 0;
      if (it.isIncomplete) {
        slices.flags[row] |= CPU_SLICE_FLAGS_INCOMPLETE;
      }
      if (it.isRealtime) {
        slices.flags[row] |= CPU_SLICE_FLAGS_REALTIME;
      }
    }
    return slices;
  }

  async onDestroy() {
    await this.trace.engine.tryQuery(
      `drop table if exists cpu_slice_${this.trackUuid}`,
    );
    this.fetcher[Symbol.dispose]();
  }

  getHeight(): number {
    return TRACK_HEIGHT;
  }

  render(trackCtx: TrackRenderContext): void {
    const {ctx, size, timescale} = trackCtx;

    // TODO: fonts and colors should come from the CSS and not hardcoded here.
    const data = this.fetcher.data;

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

    this.renderSlices(trackCtx, data);
  }

  renderSlices(
    {ctx, timescale, size, visibleWindow}: TrackRenderContext,
    data: Data,
  ): void {
    assertTrue(data.startQs.length === data.endQs.length);
    assertTrue(data.startQs.length === data.utids.length);

    const visWindowEndPx = size.width;

    ctx.textAlign = 'center';
    ctx.font = '12px Roboto Condensed';
    const charWidth = ctx.measureText('dbpqaouk').width / 8;

    const timespan = visibleWindow.toTimeSpan();

    const startTime = timespan.start;
    const endTime = timespan.end;

    const rawStartIdx = data.endQs.findIndex((end) => end >= startTime);
    const startIdx = rawStartIdx === -1 ? 0 : rawStartIdx;

    const [, rawEndIdx] = searchSegment(data.startQs, endTime);
    const endIdx = rawEndIdx === -1 ? data.startQs.length : rawEndIdx;

    for (let i = startIdx; i < endIdx; i++) {
      const tStart = Time.fromRaw(data.startQs[i]);
      let tEnd = Time.fromRaw(data.endQs[i]);
      const utid = data.utids[i];

      // If the last slice is incomplete, it should end with the end of the
      // window, else it might spill over the window and the end would not be
      // visible as a zigzag line.
      if (
        data.ids[i] === data.lastRowId &&
        data.flags[i] & CPU_SLICE_FLAGS_INCOMPLETE
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

      if (data.flags[i] & CPU_SLICE_FLAGS_INCOMPLETE) {
        drawIncompleteSlice(ctx, rectStart, MARGIN_TOP, rectWidth, RECT_HEIGHT);
      } else {
        ctx.fillRect(rectStart, MARGIN_TOP, rectWidth, RECT_HEIGHT);
      }

      // Don't render text when we have less than 5px to play with.
      if (rectWidth < 5) continue;

      // Stylize real-time threads. We don't do it when zoomed out as the
      // fillRect is expensive.
      if (data.flags[i] & CPU_SLICE_FLAGS_REALTIME) {
        ctx.fillStyle = getHatchedPattern(ctx);
        ctx.fillRect(rectStart, MARGIN_TOP, rectWidth, RECT_HEIGHT);
      }

      // TODO: consider de-duplicating this code with the copied one from
      // chrome_slices/frontend.ts.
      let title = `[utid:${utid}]`;
      let subTitle = '';
      if (threadInfo) {
        if (threadInfo.pid !== undefined && threadInfo.pid !== 0) {
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

      if (data.flags[i] & CPU_SLICE_FLAGS_REALTIME) {
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
        const [startIndex, endIndex] = searchEq(data.ids, selection.eventId);
        if (startIndex !== endIndex) {
          const tStart = Time.fromRaw(data.startQs[startIndex]);
          const tEnd = Time.fromRaw(data.endQs[startIndex]);
          const utid = data.utids[startIndex];
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
          // Draw arrow from wakeup time of current slice.
          if (selection.wakeupTs) {
            const wakeupPos = timescale.timeToPx(selection.wakeupTs);
            const latencyWidth = rectStart - wakeupPos;
            drawDoubleHeadedArrow(
              ctx,
              wakeupPos,
              MARGIN_TOP + RECT_HEIGHT,
              latencyWidth,
              latencyWidth >= 20,
            );
            // Latency time with a white semi-transparent background.
            const latency = tStart - selection.wakeupTs;
            const displayText = Duration.humanise(latency);
            const measured = ctx.measureText(displayText);
            if (latencyWidth >= measured.width + 2) {
              ctx.fillStyle = 'rgba(255,255,255,0.7)';
              ctx.fillRect(
                wakeupPos + latencyWidth / 2 - measured.width / 2 - 1,
                MARGIN_TOP + RECT_HEIGHT - 12,
                measured.width + 2,
                11,
              );
              ctx.textBaseline = 'bottom';
              ctx.fillStyle = 'black';
              ctx.fillText(
                displayText,
                wakeupPos + latencyWidth / 2,
                MARGIN_TOP + RECT_HEIGHT - 1,
              );
            }
          }
        }
      }

      // Draw diamond if the track being drawn is the cpu of the waker.
      if (this.cpu === selection.wakerCpu && selection.wakeupTs) {
        const wakeupPos = Math.floor(timescale.timeToPx(selection.wakeupTs));
        ctx.beginPath();
        ctx.moveTo(wakeupPos, MARGIN_TOP + RECT_HEIGHT / 2 + 8);
        ctx.fillStyle = 'black';
        ctx.lineTo(wakeupPos + 6, MARGIN_TOP + RECT_HEIGHT / 2);
        ctx.lineTo(wakeupPos, MARGIN_TOP + RECT_HEIGHT / 2 - 8);
        ctx.lineTo(wakeupPos - 6, MARGIN_TOP + RECT_HEIGHT / 2);
        ctx.fill();
        ctx.closePath();
      }

      if (this.utidHoveredInThisTrack !== undefined) {
        const hoveredThread = this.threads.get(this.utidHoveredInThisTrack);
        if (hoveredThread && this.mousePos !== undefined) {
          const tidText = `T: ${hoveredThread.threadName}
          [${hoveredThread.tid}]`;
          // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
          if (hoveredThread.pid) {
            const pidText = `P: ${hoveredThread.procName}
            [${hoveredThread.pid}]`;
            drawTrackHoverTooltip(ctx, this.mousePos, size, pidText, tidText);
          } else {
            drawTrackHoverTooltip(ctx, this.mousePos, size, tidText);
          }
        }
      }
    }
  }

  onMouseMove({x, y, timescale}: TrackMouseEvent) {
    const data = this.fetcher.data;
    this.mousePos = {x, y};
    if (data === undefined) return;
    if (y < MARGIN_TOP || y > MARGIN_TOP + RECT_HEIGHT) {
      this.utidHoveredInThisTrack = undefined;
      this.trace.timeline.hoveredUtid = undefined;
      this.trace.timeline.hoveredPid = undefined;
      return;
    }
    const t = timescale.pxToHpTime(x);
    let hoveredUtid = undefined;

    for (let i = 0; i < data.startQs.length; i++) {
      const tStart = Time.fromRaw(data.startQs[i]);
      const tEnd = Time.fromRaw(data.endQs[i]);
      const utid = data.utids[i];
      if (t.gte(tStart) && t.lt(tEnd)) {
        hoveredUtid = utid;
        break;
      }
    }
    this.utidHoveredInThisTrack = hoveredUtid;
    const threadInfo = exists(hoveredUtid) && this.threads.get(hoveredUtid);
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    const hoveredPid = threadInfo ? (threadInfo.pid ? threadInfo.pid : -1) : -1;
    this.trace.timeline.hoveredUtid = hoveredUtid;
    this.trace.timeline.hoveredPid = hoveredPid;
  }

  onMouseOut() {
    this.utidHoveredInThisTrack = -1;
    this.trace.timeline.hoveredUtid = undefined;
    this.trace.timeline.hoveredPid = undefined;
    this.mousePos = undefined;
  }

  onMouseClick({x, timescale}: TrackMouseEvent) {
    const data = this.fetcher.data;
    if (data === undefined) return false;
    const time = timescale.pxToHpTime(x);
    const index = search(data.startQs, time.toTime());
    const id = index === -1 ? undefined : data.ids[index];
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    if (!id || this.utidHoveredInThisTrack === -1) return false;

    this.trace.selection.selectTrackEvent(this.uri, id);
    return true;
  }

  async getSelectionDetails?(
    eventId: number,
  ): Promise<TrackEventDetails | undefined> {
    const sched = await getSched(this.trace.engine, asSchedSqlId(eventId));
    if (sched === undefined) {
      return undefined;
    }
    const wakeup = await getSchedWakeupInfo(this.trace.engine, sched);
    return {
      ts: sched.ts,
      dur: sched.dur,
      wakeupTs: wakeup?.wakeupTs,
      wakerCpu: wakeup?.wakerCpu,
    };
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
