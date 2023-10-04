// Copyright (C) 2023 The Android Open Source Project
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
import {searchEq, searchRange} from '../../base/binary_search';
import {assertTrue} from '../../base/logging';
import {duration, time, Time} from '../../base/time';
import {Actions} from '../../common/actions';
import {calcCachedBucketSize} from '../../common/cache_utils';
import {drawTrackHoverTooltip} from '../../common/canvas_utils';
import {colorForThread} from '../../common/colorizer';
import {
  LONG,
  NUM,
  QueryResult,
} from '../../common/query_result';
import {
  TrackAdapter,
  TrackControllerAdapter,
} from '../../common/track_adapter';
import {TrackData} from '../../common/track_data';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {NewTrackArgs} from '../../frontend/track';

export const PROCESS_SCHEDULING_TRACK_KIND = 'ProcessSchedulingTrack';

const MARGIN_TOP = 5;
const RECT_HEIGHT = 30;
const TRACK_HEIGHT = MARGIN_TOP * 2 + RECT_HEIGHT;

export interface Data extends TrackData {
  kind: 'slice';
  maxCpu: number;

  // Slices are stored in a columnar fashion. All fields have the same length.
  starts: BigInt64Array;
  ends: BigInt64Array;
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
export class ProcessSchedulingTrackController extends
    TrackControllerAdapter<Config, Data> {
  private maxCpu = 0;
  private maxDur = 0n;
  private cachedBucketSize = BIMath.INT64_MAX;

  async onSetup() {
    await this.createSchedView();

    const cpus = await this.engine.getCpus();

    // A process scheduling track should only exist in a trace that has cpus.
    assertTrue(cpus.length > 0);
    this.maxCpu = Math.max(...cpus) + 1;

    const result = (await this.query(`
      select ifnull(max(dur), 0) as maxDur, count(1) as count
      from ${this.tableName('process_sched')}
    `)).iter({maxDur: LONG, count: NUM});
    assertTrue(result.valid());
    this.maxDur = result.maxDur;

    const rowCount = result.count;
    const bucketSize = calcCachedBucketSize(rowCount);
    if (bucketSize === undefined) {
      return;
    }
    await this.query(`
      create table ${this.tableName('process_sched_cached')} as
      select
        (ts + ${bucketSize / 2n}) / ${bucketSize} * ${bucketSize} as cached_tsq,
        ts,
        max(dur) as dur,
        cpu,
        utid
      from ${this.tableName('process_sched')}
      group by cached_tsq, cpu
      order by cached_tsq, cpu
    `);
    this.cachedBucketSize = bucketSize;
  }

  async onBoundsChange(start: time, end: time, resolution: duration):
      Promise<Data> {
    assertTrue(this.config.upid !== null);

    // Resolution must always be a power of 2 for this logic to work
    assertTrue(BIMath.popcount(resolution) === 1, `${resolution} not pow of 2`);

    const queryRes = await this.queryData(start, end, resolution);
    const numRows = queryRes.numRows();
    const slices: Data = {
      kind: 'slice',
      start,
      end,
      resolution,
      length: numRows,
      maxCpu: this.maxCpu,
      starts: new BigInt64Array(numRows),
      ends: new BigInt64Array(numRows),
      cpus: new Uint32Array(numRows),
      utids: new Uint32Array(numRows),
    };

    const it = queryRes.iter({
      tsq: LONG,
      ts: LONG,
      dur: LONG,
      cpu: NUM,
      utid: NUM,
    });

    for (let row = 0; it.valid(); it.next(), row++) {
      const startQ = Time.fromRaw(it.tsq);
      const start = Time.fromRaw(it.ts);
      const dur = it.dur;
      const end = Time.add(start, dur);
      const minEnd = Time.add(startQ, resolution);
      const endQ = Time.max(Time.quant(end, resolution), minEnd);

      slices.starts[row] = startQ;
      slices.ends[row] = endQ;
      slices.cpus[row] = it.cpu;
      slices.utids[row] = it.utid;
      slices.end = Time.max(endQ, slices.end);
    }
    return slices;
  }

  private queryData(start: time, end: time, bucketSize: duration):
      Promise<QueryResult> {
    const isCached = this.cachedBucketSize <= bucketSize;
    const tsq = isCached ?
        `cached_tsq / ${bucketSize} * ${bucketSize}` :
        `(ts + ${bucketSize / 2n}) / ${bucketSize} * ${bucketSize}`;
    const queryTable = isCached ? this.tableName('process_sched_cached') :
                                  this.tableName('process_sched');
    const constraintColumn = isCached ? 'cached_tsq' : 'ts';

    // The mouse move handler depends on slices being sorted by cpu then tsq
    return this.query(`
      select
        ${tsq} as tsq,
        ts,
        max(dur) as dur,
        cpu,
        utid
      from ${queryTable}
      where
        ${constraintColumn} >= ${start - this.maxDur} and
        ${constraintColumn} <= ${end}
      group by tsq, cpu
      order by cpu, tsq
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

export class ProcessSchedulingTrack extends TrackAdapter<Config, Data> {
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
    const {
      visibleTimeScale,
      visibleWindowTime,
      visibleTimeSpan,
    } = globals.frontendLocalState;
    const data = this.data();

    if (data === undefined) return;  // Can't possibly draw anything.

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
        ctx,
        this.getHeight(),
        visibleTimeScale.hpTimeToPx(visibleWindowTime.start),
        visibleTimeScale.hpTimeToPx(visibleWindowTime.end),
        visibleTimeScale.timeToPx(data.start),
        visibleTimeScale.timeToPx(data.end));

    assertTrue(data.starts.length === data.ends.length);
    assertTrue(data.starts.length === data.utids.length);

    const cpuTrackHeight = Math.floor(RECT_HEIGHT / data.maxCpu);

    for (let i = 0; i < data.ends.length; i++) {
      const tStart = Time.fromRaw(data.starts[i]);
      const tEnd = Time.fromRaw(data.ends[i]);

      // Cull slices that lie completely outside the visible window
      if (!visibleTimeSpan.intersects(tStart, tEnd)) continue;

      const utid = data.utids[i];
      const cpu = data.cpus[i];

      const rectStart = visibleTimeScale.timeToPx(tStart);
      const rectEnd = visibleTimeScale.timeToPx(tEnd);
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
    const height = this.getHeight();
    if (hoveredThread !== undefined && this.mousePos !== undefined) {
      const tidText = `T: ${hoveredThread.threadName} [${hoveredThread.tid}]`;
      if (hoveredThread.pid) {
        const pidText = `P: ${hoveredThread.procName} [${hoveredThread.pid}]`;
        drawTrackHoverTooltip(ctx, this.mousePos, height, pidText, tidText);
      } else {
        drawTrackHoverTooltip(ctx, this.mousePos, height, tidText);
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
    const {visibleTimeScale} = globals.frontendLocalState;
    const t = visibleTimeScale.pxToHpTime(pos.x).toTime('floor');

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
