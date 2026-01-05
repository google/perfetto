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
import {assertExists, assertTrue} from '../../base/logging';
import {duration, time, Time} from '../../base/time';
import {Color} from '../../base/color';
import m from 'mithril';
import {colorForThread} from '../../components/colorizer';
import {TrackData} from '../../components/tracks/track_data';
import {TimelineFetcher} from '../../components/tracks/track_helper';
import {checkerboardExcept} from '../../components/checkerboard';
import {TrackRenderer} from '../../public/track';
import {LONG, NUM, QueryResult} from '../../trace_processor/query_result';
import {uuidv4Sql} from '../../base/uuid';
import {TrackMouseEvent, TrackRenderContext} from '../../public/track';
import {Point2D} from '../../base/geom';
import {Trace} from '../../public/trace';
import {ThreadMap} from '../dev.perfetto.Thread/threads';
import {AsyncDisposableStack} from '../../base/disposable_stack';
import {
  createPerfettoTable,
  createVirtualTable,
} from '../../trace_processor/sql_utils';

export const PROCESS_SCHEDULING_TRACK_KIND = 'ProcessSchedulingTrack';

const MARGIN_TOP = 5;
const RECT_HEIGHT = 30;
const TRACK_HEIGHT = MARGIN_TOP * 2 + RECT_HEIGHT;

interface Data extends TrackData {
  kind: 'slice';
  maxCpu: number;

  // Slices are stored in a columnar fashion. All fields have the same length.
  counts: Uint32Array;
  starts: BigInt64Array;
  ends: BigInt64Array;
  utids: Uint32Array;
  cpus: Uint32Array;
}

export interface Config {
  pidForColor: bigint | number;
  upid: number | null;
  utid: number | null;
}

export class ProcessSchedulingTrack implements TrackRenderer {
  private mousePos?: Point2D;
  private utidHoveredInThisTrack = -1;
  private countHoveredInThisTrack = -1;
  private fetcher = new TimelineFetcher(this.onBoundsChange.bind(this));
  private trackUuid = uuidv4Sql();

  constructor(
    private readonly trace: Trace,
    private readonly config: Config,
    private readonly cpuCount: number,
    private readonly threads: ThreadMap,
  ) {}

  async onCreate(): Promise<void> {
    const getQuery = () => {
      if (this.config.upid !== null) {
        // TODO(lalitm): remove the harcoding of the cross join here.
        return `
          select
            s.id,
            s.ts,
            s.dur,
            c.cpu
          from thread t
          cross join sched s using (utid)
          cross join cpu c using (ucpu)
          where
            not t.is_idle and
            t.upid = ${this.config.upid}
          order by ts
        `;
      }
      assertExists(this.config.utid);
      return `
        select
          s.id,
          s.ts,
          s.dur,
          c.cpu
        from sched s
        cross join cpu c using (ucpu)
        where
          s.utid = ${this.config.utid}
      `;
    };

    const trash = new AsyncDisposableStack();
    trash.use(
      await createPerfettoTable({
        engine: this.trace.engine,
        name: `tmp_${this.trackUuid}`,
        as: getQuery(),
      }),
    );
    await createVirtualTable({
      engine: this.trace.engine,
      name: `process_scheduling_${this.trackUuid}`,
      using: `__intrinsic_slice_mipmap((
        select
          s.id,
          s.ts,
          iif(
            s.dur = -1,
            ifnull(
              (
                select n.ts
                from tmp_${this.trackUuid} n
                where n.ts > s.ts and n.cpu = s.cpu
                order by ts
                limit 1
              ),
              trace_end()
            ) - s.ts,
            s.dur
          ) as dur,
          s.cpu as depth
        from tmp_${this.trackUuid} s
      ))`,
    });
    await trash.asyncDispose();
  }

  async onUpdate({
    visibleWindow,
    resolution,
  }: TrackRenderContext): Promise<void> {
    await this.fetcher.requestData(visibleWindow.toTimeSpan(), resolution);
  }

  async onDestroy(): Promise<void> {
    this.fetcher[Symbol.dispose]();
    await this.trace.engine.tryQuery(`
      drop table process_scheduling_${this.trackUuid}
    `);
  }

  async onBoundsChange(
    start: time,
    end: time,
    resolution: duration,
  ): Promise<Data> {
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
      maxCpu: this.cpuCount,
      counts: new Uint32Array(numRows),
      starts: new BigInt64Array(numRows),
      ends: new BigInt64Array(numRows),
      cpus: new Uint32Array(numRows),
      utids: new Uint32Array(numRows),
    };

    const it = queryRes.iter({
      count: NUM,
      ts: LONG,
      dur: LONG,
      cpu: NUM,
      utid: NUM,
    });

    for (let row = 0; it.valid(); it.next(), row++) {
      const start = Time.fromRaw(it.ts);
      const dur = it.dur;
      const end = Time.add(start, dur);

      slices.counts[row] = it.count;
      slices.starts[row] = start;
      slices.ends[row] = end;
      slices.cpus[row] = it.cpu;
      slices.utids[row] = it.utid;
      slices.end = Time.max(end, slices.end);
    }
    return slices;
  }

  private async queryData(
    start: time,
    end: time,
    bucketSize: duration,
  ): Promise<QueryResult> {
    return this.trace.engine.query(`
      select
        (z.ts / ${bucketSize}) * ${bucketSize} as ts,
        iif(s.dur = -1, s.dur, max(z.dur, ${bucketSize})) as dur,
        s.id,
        z.count,
        z.depth as cpu,
        utid
      from process_scheduling_${this.trackUuid}(
        ${start}, ${end}, ${bucketSize}
      ) z
      cross join sched s using (id)
    `);
  }

  getHeight(): number {
    return TRACK_HEIGHT;
  }

  renderTooltip(): m.Children {
    if (this.utidHoveredInThisTrack === -1 || this.mousePos === undefined) {
      return undefined;
    }

    const hoveredThread = this.threads.get(this.utidHoveredInThisTrack);
    if (!hoveredThread) {
      return undefined;
    }

    const tidText = `T: ${hoveredThread.threadName} [${hoveredThread.tid}]`;

    const count = this.countHoveredInThisTrack;
    const countDiv = count > 1 && m('div', `and ${count - 1} other events`);
    if (hoveredThread.pid !== undefined) {
      const pidText = `P: ${hoveredThread.procName} [${hoveredThread.pid}]`;
      return m('.tooltip', [m('div', pidText), m('div', tidText), countDiv]);
    } else {
      return m('.tooltip', tidText, countDiv);
    }
  }

  render({ctx, size, timescale, visibleWindow}: TrackRenderContext): void {
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

    assertTrue(data.starts.length === data.ends.length);
    assertTrue(data.starts.length === data.utids.length);

    const cpuTrackHeight = Math.floor(RECT_HEIGHT / data.maxCpu);

    for (let i = 0; i < data.ends.length; i++) {
      const tStart = Time.fromRaw(data.starts[i]);
      const tEnd = Time.fromRaw(data.ends[i]);

      // Cull slices that lie completely outside the visible window
      if (!visibleWindow.overlaps(tStart, tEnd)) continue;

      const utid = data.utids[i];
      const cpu = data.cpus[i];

      const rectStart = Math.floor(timescale.timeToPx(tStart));
      const rectEnd = Math.floor(timescale.timeToPx(tEnd));
      const rectWidth = Math.max(1, rectEnd - rectStart);

      const threadInfo = this.threads.get(utid);
      // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
      const pid = (threadInfo ? threadInfo.pid : -1) || -1;

      const isHovering = this.trace.timeline.hoveredUtid !== undefined;
      const isThreadHovered = this.trace.timeline.hoveredUtid === utid;
      const isProcessHovered = this.trace.timeline.hoveredPid === pid;
      const colorScheme = colorForThread(threadInfo);
      let color: Color;
      if (isHovering && !isThreadHovered) {
        if (!isProcessHovered) {
          color = colorScheme.disabled;
        } else {
          color = colorScheme.variant;
        }
      } else {
        color = colorScheme.base;
      }
      ctx.fillStyle = color.cssString;
      const y = MARGIN_TOP + cpuTrackHeight * cpu + cpu;
      ctx.fillRect(rectStart, y, rectWidth, cpuTrackHeight);
    }
  }

  onMouseMove({x, y, timescale}: TrackMouseEvent) {
    const data = this.fetcher.data;
    this.mousePos = {x, y};
    if (data === undefined) return;
    if (y < MARGIN_TOP || y > MARGIN_TOP + RECT_HEIGHT) {
      this.utidHoveredInThisTrack = -1;
      this.countHoveredInThisTrack = -1;
      this.trace.timeline.hoveredUtid = undefined;
      this.trace.timeline.hoveredPid = undefined;
      return;
    }

    const cpuTrackHeight = Math.floor(RECT_HEIGHT / data.maxCpu);
    const cpu = Math.floor((y - MARGIN_TOP) / (cpuTrackHeight + 1));
    const t = timescale.pxToHpTime(x).toTime('floor');

    const [i, j] = searchRange(data.starts, t, searchEq(data.cpus, cpu));
    if (i === j || i >= data.starts.length || t > data.ends[i]) {
      this.utidHoveredInThisTrack = -1;
      this.countHoveredInThisTrack = -1;
      this.trace.timeline.hoveredUtid = undefined;
      this.trace.timeline.hoveredPid = undefined;
      return;
    }

    const utid = data.utids[i];
    const count = data.counts[i];
    this.utidHoveredInThisTrack = utid;
    this.countHoveredInThisTrack = count;
    const threadInfo = this.threads.get(utid);
    this.trace.timeline.hoveredUtid = utid;
    this.trace.timeline.hoveredPid = threadInfo?.pid;

    // Trigger redraw to update tooltip
    m.redraw();
  }

  onMouseOut() {
    this.utidHoveredInThisTrack = -1;
    this.trace.timeline.hoveredUtid = undefined;
    this.trace.timeline.hoveredPid = undefined;
    this.mousePos = undefined;
  }
}
