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

import {searchEq, searchRange, searchSegment} from '../../base/binary_search';
import {assertTrue} from '../../base/logging';
import {Actions} from '../../common/actions';
import {colorForThread} from '../../common/colorizer';
import {PluginContext} from '../../common/plugin_api';
import {NUM, QueryResult} from '../../common/query_result';
import {fromNs, toNs} from '../../common/time';
import {TrackData} from '../../common/track_data';
import {
  TrackController,
} from '../../controller/track_controller';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {NewTrackArgs, Track} from '../../frontend/track';

export const PROCESS_SCHEDULING_TRACK_KIND = 'ProcessSchedulingTrack';

export interface Data extends TrackData {
  kind: 'slice';
  maxCpu: number;

  // Slices are stored in a columnar fashion. All fields have the same length.
  starts: Float64Array;
  ends: Float64Array;
  utids: Uint32Array;
  cpus: Uint32Array;
}

export interface Config {
  pidForColor: number;
  upid: null|number;
  utid: number;
}

// This summary is displayed for any processes that have CPU scheduling activity
// associated with them.
class ProcessSchedulingTrackController extends TrackController<Config, Data> {
  static readonly kind = PROCESS_SCHEDULING_TRACK_KIND;

  private maxCpu = 0;
  private maxDurNs = 0;
  private cachedBucketNs = Number.MAX_SAFE_INTEGER;

  async onSetup() {
    await this.createSchedView();

    const cpus = await this.engine.getCpus();

    // A process scheduling track should only exist in a trace that has cpus.
    assertTrue(cpus.length > 0);
    this.maxCpu = Math.max(...cpus) + 1;

    const result = (await this.query(`
      select ifnull(max(dur), 0) as maxDur, count(1) as count
      from ${this.tableName('process_sched')}
    `)).iter({maxDur: NUM, count: NUM});
    assertTrue(result.valid());
    this.maxDurNs = result.maxDur;

    const rowCount = result.count;
    const bucketNs = this.cachedBucketSizeNs(rowCount);
    if (bucketNs === undefined) {
      return;
    }
    await this.query(`
      create table ${this.tableName('process_sched_cached')} as
      select
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as cached_tsq,
        ts,
        max(dur) as dur,
        cpu,
        utid
      from ${this.tableName('process_sched')}
      group by cached_tsq, cpu
      order by cached_tsq, cpu
    `);
    this.cachedBucketNs = bucketNs;
  }

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    assertTrue(this.config.upid !== null);

    // The resolution should always be a power of two for the logic of this
    // function to make sense.
    const resolutionNs = toNs(resolution);
    assertTrue(Math.log2(resolutionNs) % 1 === 0);

    const startNs = toNs(start);
    const endNs = toNs(end);

    // ns per quantization bucket (i.e. ns per pixel). /2 * 2 is to force it to
    // be an even number, so we can snap in the middle.
    const bucketNs =
        Math.max(Math.round(resolutionNs * this.pxSize() / 2) * 2, 1);

    const queryRes = await this.queryData(startNs, endNs, bucketNs);
    const numRows = queryRes.numRows();
    const slices: Data = {
      kind: 'slice',
      start,
      end,
      resolution,
      length: numRows,
      maxCpu: this.maxCpu,
      starts: new Float64Array(numRows),
      ends: new Float64Array(numRows),
      cpus: new Uint32Array(numRows),
      utids: new Uint32Array(numRows),
    };

    const it = queryRes.iter({
      tsq: NUM,
      ts: NUM,
      dur: NUM,
      cpu: NUM,
      utid: NUM,
    });

    for (let row = 0; it.valid(); it.next(), row++) {
      const startNsQ = it.tsq;
      const startNs = it.ts;
      const durNs = it.dur;
      const endNs = startNs + durNs;

      let endNsQ = Math.floor((endNs + bucketNs / 2 - 1) / bucketNs) * bucketNs;
      endNsQ = Math.max(endNsQ, startNsQ + bucketNs);

      slices.starts[row] = fromNs(startNsQ);
      slices.ends[row] = fromNs(endNsQ);
      slices.cpus[row] = it.cpu;
      slices.utids[row] = it.utid;
      slices.end = Math.max(slices.ends[row], slices.end);
    }
    return slices;
  }

  private queryData(startNs: number, endNs: number, bucketNs: number):
      Promise<QueryResult> {
    const isCached = this.cachedBucketNs <= bucketNs;
    const tsq = isCached ? `cached_tsq / ${bucketNs} * ${bucketNs}` :
                           `(ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs}`;
    const queryTable = isCached ? this.tableName('process_sched_cached') :
                                  this.tableName('process_sched');
    const constraintColumn = isCached ? 'cached_tsq' : 'ts';
    return this.query(`
      select
        ${tsq} as tsq,
        ts,
        max(dur) as dur,
        cpu,
        utid
      from ${queryTable}
      where
        ${constraintColumn} >= ${startNs - this.maxDurNs} and
        ${constraintColumn} <= ${endNs}
      group by tsq, cpu
      order by tsq, cpu
    `);
  }

  private async createSchedView() {
    await this.query(`
      create view ${this.tableName('process_sched')} as
      select ts, dur, cpu, utid
      from experimental_sched_upid
      where
        utid != 0 and
        upid = ${this.config.upid}
    `);
  }
}

const MARGIN_TOP = 5;
const RECT_HEIGHT = 30;
const TRACK_HEIGHT = MARGIN_TOP * 2 + RECT_HEIGHT;

class ProcessSchedulingTrack extends Track<Config, Data> {
  static readonly kind = PROCESS_SCHEDULING_TRACK_KIND;
  static create(args: NewTrackArgs): ProcessSchedulingTrack {
    return new ProcessSchedulingTrack(args);
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
    const {timeScale, visibleWindowTime} = globals.frontendLocalState;
    const data = this.data();

    if (data === undefined) return;  // Can't possibly draw anything.

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
        ctx,
        this.getHeight(),
        timeScale.timeToPx(visibleWindowTime.start),
        timeScale.timeToPx(visibleWindowTime.end),
        timeScale.timeToPx(data.start),
        timeScale.timeToPx(data.end));

    assertTrue(data.starts.length === data.ends.length);
    assertTrue(data.starts.length === data.utids.length);

    const rawStartIdx =
        data.ends.findIndex((end) => end >= visibleWindowTime.start);
    const startIdx = rawStartIdx === -1 ? data.starts.length : rawStartIdx;

    const [, rawEndIdx] = searchSegment(data.starts, visibleWindowTime.end);
    const endIdx = rawEndIdx === -1 ? data.starts.length : rawEndIdx;

    const cpuTrackHeight = Math.floor(RECT_HEIGHT / data.maxCpu);

    for (let i = startIdx; i < endIdx; i++) {
      const tStart = data.starts[i];
      const tEnd = data.ends[i];
      const utid = data.utids[i];
      const cpu = data.cpus[i];

      const rectStart = timeScale.timeToPx(tStart);
      const rectEnd = timeScale.timeToPx(tEnd);
      const rectWidth = rectEnd - rectStart;
      if (rectWidth < 0.3) continue;

      const threadInfo = globals.threads.get(utid);
      const pid = (threadInfo ? threadInfo.pid : -1) || -1;

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
      const y = MARGIN_TOP + cpuTrackHeight * cpu + cpu;
      ctx.fillRect(rectStart, y, rectEnd - rectStart, cpuTrackHeight);
    }

    const hoveredThread = globals.threads.get(this.utidHoveredInThisTrack);
    if (hoveredThread !== undefined && this.mousePos !== undefined) {
      const tidText = `T: ${hoveredThread.threadName} [${hoveredThread.tid}]`;
      if (hoveredThread.pid) {
        const pidText = `P: ${hoveredThread.procName} [${hoveredThread.pid}]`;
        this.drawTrackHoverTooltip(ctx, this.mousePos, pidText, tidText);
      } else {
        this.drawTrackHoverTooltip(ctx, this.mousePos, tidText);
      }
    }
  }

  onMouseMove(pos: {x: number, y: number}) {
    const data = this.data();
    this.mousePos = pos;
    if (data === undefined) return;
    if (pos.y < MARGIN_TOP || pos.y > MARGIN_TOP + RECT_HEIGHT) {
      this.utidHoveredInThisTrack = -1;
      globals.dispatch(Actions.setHoveredUtidAndPid({utid: -1, pid: -1}));
      return;
    }

    const cpuTrackHeight = Math.floor(RECT_HEIGHT / data.maxCpu);
    const cpu = Math.floor((pos.y - MARGIN_TOP) / (cpuTrackHeight + 1));
    const {timeScale} = globals.frontendLocalState;
    const t = timeScale.pxToTime(pos.x);

    const [i, j] = searchRange(data.starts, t, searchEq(data.cpus, cpu));
    if (i === j || i >= data.starts.length || t > data.ends[i]) {
      this.utidHoveredInThisTrack = -1;
      globals.dispatch(Actions.setHoveredUtidAndPid({utid: -1, pid: -1}));
      return;
    }

    const utid = data.utids[i];
    this.utidHoveredInThisTrack = utid;
    const threadInfo = globals.threads.get(utid);
    const pid = threadInfo ? (threadInfo.pid ? threadInfo.pid : -1) : -1;
    globals.dispatch(Actions.setHoveredUtidAndPid({utid, pid}));
  }

  onMouseOut() {
    this.utidHoveredInThisTrack = -1;
    globals.dispatch(Actions.setHoveredUtidAndPid({utid: -1, pid: -1}));
    this.mousePos = undefined;
  }
}

export function activate(ctx: PluginContext) {
  ctx.registerTrackController(ProcessSchedulingTrackController);
  ctx.registerTrack(ProcessSchedulingTrack);
}

export const plugin = {
  pluginId: 'perfetto.ProcessScheduling',
  activate,
};
