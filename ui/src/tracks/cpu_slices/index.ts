// Copyright (C) 2021 The Android Open Source Project
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
import {assertTrue} from '../../base/logging';
import {Duration, duration, Time, time} from '../../base/time';
import {Actions} from '../../common/actions';
import {calcCachedBucketSize} from '../../common/cache_utils';
import {
  cropText,
  drawDoubleHeadedArrow,
  drawIncompleteSlice,
  drawTrackHoverTooltip,
} from '../../common/canvas_utils';
import {colorForThread} from '../../common/colorizer';
import {LONG, NUM, STR_NULL} from '../../common/query_result';
import {
  TrackAdapter,
  TrackControllerAdapter,
  TrackWithControllerAdapter,
} from '../../common/track_adapter';
import {TrackData} from '../../common/track_data';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {NewTrackArgs} from '../../frontend/track';
import {
  EngineProxy,
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';

export const CPU_SLICE_TRACK_KIND = 'CpuSliceTrack';

export interface Data extends TrackData {
  // Slices are stored in a columnar fashion. All fields have the same length.
  ids: Float64Array;
  starts: BigInt64Array;
  ends: BigInt64Array;
  utids: Uint32Array;
  isIncomplete: Uint8Array;
  lastRowId: number;
}

export interface Config {
  cpu: number;
}

class CpuSliceTrackController extends TrackControllerAdapter<Config, Data> {
  private cachedBucketSize = BIMath.INT64_MAX;
  private maxDur: duration = 0n;
  private lastRowId = -1;

  async onSetup() {
    await this.query(`
      create view ${this.tableName('sched')} as
      select
        ts,
        dur,
        utid,
        id,
        dur = -1 as isIncomplete
      from sched
      where cpu = ${this.config.cpu} and utid != 0
    `);

    const queryRes = await this.query(`
      select ifnull(max(dur), 0) as maxDur, count(1) as rowCount
      from ${this.tableName('sched')}
    `);

    const queryLastSlice = await this.query(`
    select ifnull(max(id), -1) as lastSliceId from ${this.tableName('sched')}
    `);
    this.lastRowId = queryLastSlice.firstRow({lastSliceId: NUM}).lastSliceId;

    const row = queryRes.firstRow({maxDur: LONG, rowCount: NUM});
    this.maxDur = row.maxDur;
    const rowCount = row.rowCount;
    const bucketSize = calcCachedBucketSize(rowCount);
    if (bucketSize === undefined) {
      return;
    }

    await this.query(`
      create table ${this.tableName('sched_cached')} as
      select
        (ts + ${bucketSize / 2n}) / ${bucketSize} * ${bucketSize} as cached_tsq,
        ts,
        max(dur) as dur,
        utid,
        id,
        isIncomplete
      from ${this.tableName('sched')}
      group by cached_tsq, isIncomplete
      order by cached_tsq
    `);
    this.cachedBucketSize = bucketSize;
  }

  async onBoundsChange(start: time, end: time, resolution: duration):
      Promise<Data> {
    assertTrue(BIMath.popcount(resolution) === 1, `${resolution} not pow of 2`);

    const isCached = this.cachedBucketSize <= resolution;
    const queryTsq = isCached ?
        `cached_tsq / ${resolution} * ${resolution}` :
        `(ts + ${resolution / 2n}) / ${resolution} * ${resolution}`;
    const queryTable =
        isCached ? this.tableName('sched_cached') : this.tableName('sched');
    const constraintColumn = isCached ? 'cached_tsq' : 'ts';

    const queryRes = await this.query(`
      select
        ${queryTsq} as tsq,
        ts,
        max(dur) as dur,
        utid,
        id,
        isIncomplete
      from ${queryTable}
      where
        ${constraintColumn} >= ${start - this.maxDur} and
        ${constraintColumn} <= ${end}
      group by tsq, isIncomplete
      order by tsq
    `);

    const numRows = queryRes.numRows();
    const slices: Data = {
      start,
      end,
      resolution,
      length: numRows,
      lastRowId: this.lastRowId,
      ids: new Float64Array(numRows),
      starts: new BigInt64Array(numRows),
      ends: new BigInt64Array(numRows),
      utids: new Uint32Array(numRows),
      isIncomplete: new Uint8Array(numRows),
    };

    const it = queryRes.iter({
      tsq: LONG,
      ts: LONG,
      dur: LONG,
      utid: NUM,
      id: NUM,
      isIncomplete: NUM,
    });
    for (let row = 0; it.valid(); it.next(), row++) {
      const startQ = it.tsq;
      const start = it.ts;
      const dur = it.dur;
      const end = start + dur;

      // If the slice is incomplete, the end calculated later.
      if (!it.isIncomplete) {
        const minEnd = startQ + resolution;
        const endQ = BIMath.max(BIMath.quant(end, resolution), minEnd);
        slices.ends[row] = endQ;
      }

      slices.starts[row] = startQ;
      slices.utids[row] = it.utid;
      slices.ids[row] = it.id;
      slices.isIncomplete[row] = it.isIncomplete;
    }

    // If the slice is incomplete and it is the last slice in the track, the end
    // of the slice would be the end of the visible window. Otherwise we end the
    // slice with the beginning the next one.
    for (let row = 0; row < slices.length; row++) {
      if (!slices.isIncomplete[row]) {
        continue;
      }
      const endTime = row === slices.length - 1 ? end : slices.starts[row + 1];
      const minEnd = slices.starts[row] + resolution;
      const endQ = BIMath.max(BIMath.quant(endTime, resolution), minEnd);
      slices.ends[row] = endQ;
    }
    return slices;
  }

  async onDestroy() {
    await this.query(`drop table if exists ${this.tableName('sched_cached')}`);
  }
}

const MARGIN_TOP = 3;
const RECT_HEIGHT = 24;
const TRACK_HEIGHT = MARGIN_TOP * 2 + RECT_HEIGHT;

class CpuSliceTrack extends TrackAdapter<Config, Data> {
  static create(args: NewTrackArgs): CpuSliceTrack {
    return new CpuSliceTrack(args);
  }

  private mousePos?: {x: number, y: number};
  private utidHoveredInThisTrack = -1;

  constructor(args: NewTrackArgs) {
    super(args);
  }

  getHeight(): number {
    return TRACK_HEIGHT;
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    // TODO: fonts and colors should come from the CSS and not hardcoded here.
    const {visibleTimeScale, windowSpan} = globals.frontendLocalState;
    const data = this.data();

    if (data === undefined) return;  // Can't possibly draw anything.

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
        ctx,
        this.getHeight(),
        windowSpan.start,
        windowSpan.end,
        visibleTimeScale.timeToPx(data.start),
        visibleTimeScale.timeToPx(data.end));

    this.renderSlices(ctx, data);
  }

  renderSlices(ctx: CanvasRenderingContext2D, data: Data): void {
    const {
      visibleTimeScale,
      visibleTimeSpan,
      visibleWindowTime,
    } = globals.frontendLocalState;
    assertTrue(data.starts.length === data.ends.length);
    assertTrue(data.starts.length === data.utids.length);

    const visWindowEndPx = visibleTimeScale.hpTimeToPx(visibleWindowTime.end);

    ctx.textAlign = 'center';
    ctx.font = '12px Roboto Condensed';
    const charWidth = ctx.measureText('dbpqaouk').width / 8;

    const startTime = visibleTimeSpan.start;
    const endTime = visibleTimeSpan.end;

    const rawStartIdx = data.ends.findIndex((end) => end >= startTime);
    const startIdx = rawStartIdx === -1 ? 0 : rawStartIdx;

    const [, rawEndIdx] = searchSegment(data.starts, endTime);
    const endIdx = rawEndIdx === -1 ? data.starts.length : rawEndIdx;

    for (let i = startIdx; i < endIdx; i++) {
      const tStart = Time.fromRaw(data.starts[i]);
      let tEnd = Time.fromRaw(data.ends[i]);
      const utid = data.utids[i];

      // If the last slice is incomplete, it should end with the end of the
      // window, else it might spill over the window and the end would not be
      // visible as a zigzag line.
      if (data.ids[i] === data.lastRowId && data.isIncomplete[i]) {
        tEnd = endTime;
      }
      const rectStart = visibleTimeScale.timeToPx(tStart);
      const rectEnd = visibleTimeScale.timeToPx(tEnd);
      const rectWidth = Math.max(1, rectEnd - rectStart);

      const threadInfo = globals.threads.get(utid);
      const pid = threadInfo && threadInfo.pid ? threadInfo.pid : -1;

      const isHovering = globals.state.hoveredUtid !== -1;
      const isThreadHovered = globals.state.hoveredUtid === utid;
      const isProcessHovered = globals.state.hoveredPid === pid;
      const color = colorForThread(threadInfo);
      if (isHovering && !isThreadHovered) {
        if (!isProcessHovered) {
          color.l = 90;
          color.s = 0;
        } else {
          color.l = Math.min(color.l + 30, 80);
          color.s -= 20;
        }
      } else {
        color.l = Math.min(color.l + 10, 60);
        color.s -= 20;
      }
      ctx.fillStyle = `hsl(${color.h}, ${color.s}%, ${color.l}%)`;
      if (data.isIncomplete[i]) {
        drawIncompleteSlice(ctx, rectStart, MARGIN_TOP, rectWidth, RECT_HEIGHT);
      } else {
        ctx.fillRect(rectStart, MARGIN_TOP, rectWidth, RECT_HEIGHT);
      }

      // Don't render text when we have less than 5px to play with.
      if (rectWidth < 5) continue;

      // TODO: consider de-duplicating this code with the copied one from
      // chrome_slices/frontend.ts.
      let title = `[utid:${utid}]`;
      let subTitle = '';
      if (threadInfo) {
        if (threadInfo.pid) {
          let procName = threadInfo.procName || '';
          if (procName.startsWith('/')) {  // Remove folder paths from name
            procName = procName.substring(procName.lastIndexOf('/') + 1);
          }
          title = `${procName} [${threadInfo.pid}]`;
          subTitle = `${threadInfo.threadName} [${threadInfo.tid}]`;
        } else {
          title = `${threadInfo.threadName} [${threadInfo.tid}]`;
        }
      }
      const right = Math.min(visWindowEndPx, rectEnd);
      const left = Math.max(rectStart, 0);
      const visibleWidth = Math.max(right - left, 1);
      title = cropText(title, charWidth, visibleWidth);
      subTitle = cropText(subTitle, charWidth, visibleWidth);
      const rectXCenter = left + visibleWidth / 2;
      ctx.fillStyle = '#fff';
      ctx.font = '12px Roboto Condensed';
      ctx.fillText(title, rectXCenter, MARGIN_TOP + RECT_HEIGHT / 2 - 1);
      ctx.fillStyle = 'rgba(255, 255, 255, 0.6)';
      ctx.font = '10px Roboto Condensed';
      ctx.fillText(subTitle, rectXCenter, MARGIN_TOP + RECT_HEIGHT / 2 + 9);
    }

    const selection = globals.state.currentSelection;
    const details = globals.sliceDetails;
    if (selection !== null && selection.kind === 'SLICE') {
      const [startIndex, endIndex] = searchEq(data.ids, selection.id);
      if (startIndex !== endIndex) {
        const tStart = Time.fromRaw(data.starts[startIndex]);
        const tEnd = Time.fromRaw(data.ends[startIndex]);
        const utid = data.utids[startIndex];
        const color = colorForThread(globals.threads.get(utid));
        const rectStart = visibleTimeScale.timeToPx(tStart);
        const rectEnd = visibleTimeScale.timeToPx(tEnd);
        const rectWidth = Math.max(1, rectEnd - rectStart);

        // Draw a rectangle around the slice that is currently selected.
        ctx.strokeStyle = `hsl(${color.h}, ${color.s}%, 30%)`;
        ctx.beginPath();
        ctx.lineWidth = 3;
        ctx.strokeRect(rectStart, MARGIN_TOP - 1.5, rectWidth, RECT_HEIGHT + 3);
        ctx.closePath();
        // Draw arrow from wakeup time of current slice.
        if (details.wakeupTs) {
          const wakeupPos = visibleTimeScale.timeToPx(details.wakeupTs);
          const latencyWidth = rectStart - wakeupPos;
          drawDoubleHeadedArrow(
              ctx,
              wakeupPos,
              MARGIN_TOP + RECT_HEIGHT,
              latencyWidth,
              latencyWidth >= 20);
          // Latency time with a white semi-transparent background.
          const latency = tStart - details.wakeupTs;
          const displayText = Duration.humanise(latency);
          const measured = ctx.measureText(displayText);
          if (latencyWidth >= measured.width + 2) {
            ctx.fillStyle = 'rgba(255,255,255,0.7)';
            ctx.fillRect(
                wakeupPos + latencyWidth / 2 - measured.width / 2 - 1,
                MARGIN_TOP + RECT_HEIGHT - 12,
                measured.width + 2,
                11);
            ctx.textBaseline = 'bottom';
            ctx.fillStyle = 'black';
            ctx.fillText(
                displayText,
                wakeupPos + (latencyWidth) / 2,
                MARGIN_TOP + RECT_HEIGHT - 1);
          }
        }
      }

      // Draw diamond if the track being drawn is the cpu of the waker.
      if (this.config.cpu === details.wakerCpu && details.wakeupTs) {
        const wakeupPos =
            Math.floor(visibleTimeScale.timeToPx(details.wakeupTs));
        ctx.beginPath();
        ctx.moveTo(wakeupPos, MARGIN_TOP + RECT_HEIGHT / 2 + 8);
        ctx.fillStyle = 'black';
        ctx.lineTo(wakeupPos + 6, MARGIN_TOP + RECT_HEIGHT / 2);
        ctx.lineTo(wakeupPos, MARGIN_TOP + RECT_HEIGHT / 2 - 8);
        ctx.lineTo(wakeupPos - 6, MARGIN_TOP + RECT_HEIGHT / 2);
        ctx.fill();
        ctx.closePath();
      }
    }

    const hoveredThread = globals.threads.get(this.utidHoveredInThisTrack);
    const maxHeight = this.getHeight();
    if (hoveredThread !== undefined && this.mousePos !== undefined) {
      const tidText = `T: ${hoveredThread.threadName}
      [${hoveredThread.tid}]`;
      if (hoveredThread.pid) {
        const pidText = `P: ${hoveredThread.procName}
        [${hoveredThread.pid}]`;
        drawTrackHoverTooltip(ctx, this.mousePos, maxHeight, pidText, tidText);
      } else {
        drawTrackHoverTooltip(ctx, this.mousePos, maxHeight, tidText);
      }
    }
  }

  onMouseMove(pos: {x: number, y: number}) {
    const data = this.data();
    this.mousePos = pos;
    if (data === undefined) return;
    const {visibleTimeScale} = globals.frontendLocalState;
    if (pos.y < MARGIN_TOP || pos.y > MARGIN_TOP + RECT_HEIGHT) {
      this.utidHoveredInThisTrack = -1;
      globals.dispatch(Actions.setHoveredUtidAndPid({utid: -1, pid: -1}));
      return;
    }
    const t = visibleTimeScale.pxToHpTime(pos.x);
    let hoveredUtid = -1;

    for (let i = 0; i < data.starts.length; i++) {
      const tStart = Time.fromRaw(data.starts[i]);
      const tEnd = Time.fromRaw(data.ends[i]);
      const utid = data.utids[i];
      if (t.gte(tStart) && t.lt(tEnd)) {
        hoveredUtid = utid;
        break;
      }
    }
    this.utidHoveredInThisTrack = hoveredUtid;
    const threadInfo = globals.threads.get(hoveredUtid);
    const hoveredPid = threadInfo ? (threadInfo.pid ? threadInfo.pid : -1) : -1;
    globals.dispatch(
        Actions.setHoveredUtidAndPid({utid: hoveredUtid, pid: hoveredPid}));
  }

  onMouseOut() {
    this.utidHoveredInThisTrack = -1;
    globals.dispatch(Actions.setHoveredUtidAndPid({utid: -1, pid: -1}));
    this.mousePos = undefined;
  }

  onMouseClick({x}: {x: number}) {
    const data = this.data();
    if (data === undefined) return false;
    const {visibleTimeScale} = globals.frontendLocalState;
    const time = visibleTimeScale.pxToHpTime(x);
    const index = search(data.starts, time.toTime());
    const id = index === -1 ? undefined : data.ids[index];
    if (!id || this.utidHoveredInThisTrack === -1) return false;
    globals.makeSelection(Actions.selectSlice({id, trackKey: this.trackKey}));
    return true;
  }
}

class CpuSlices implements Plugin {
  onActivate(_ctx: PluginContext): void {
    // No-op
  }

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const cpus = await ctx.engine.getCpus();
    const cpuToSize = await this.guessCpuSizes(ctx.engine);

    for (const cpu of cpus) {
      const size = cpuToSize.get(cpu);
      const uri = `perfetto.CpuSlices#cpu${cpu}`;
      const name = size === undefined ? `Cpu ${cpu}` : `Cpu ${cpu} (${size})`;
      const config: Config = {cpu};
      ctx.registerStaticTrack({
        uri,
        displayName: name,
        kind: CPU_SLICE_TRACK_KIND,
        cpu,
        track: ({trackKey}) => {
          return new TrackWithControllerAdapter<Config, Data>(
              ctx.engine,
              trackKey,
              config,
              CpuSliceTrack,
              CpuSliceTrackController);
        },
      });
    }
  }

  async guessCpuSizes(engine: EngineProxy): Promise<Map<number, string>> {
    const cpuToSize = new Map<number, string>();
    await engine.query(`
      INCLUDE PERFETTO MODULE common.cpus;
    `);
    const result = await engine.query(`
      SELECT cpu, GUESS_CPU_SIZE(cpu) as size FROM cpu_counter_track;
    `);

    const it = result.iter({
      cpu: NUM,
      size: STR_NULL,
    });

    for (; it.valid(); it.next()) {
      const size = it.size;
      if (size !== null) {
        cpuToSize.set(it.cpu, size);
      }
    }

    return cpuToSize;
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.CpuSlices',
  plugin: CpuSlices,
};
