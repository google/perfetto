// Copyright (C) 2025 The Android Open Source Project
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
import {searchEq, searchRange} from '../../base/binary_search';
import {assertExists, assertTrue} from '../../base/logging';
import {duration, time, Time} from '../../base/time';
import m from 'mithril';
import {colorForThread, colorForTid} from '../../components/colorizer';
import {ColorScheme} from '../../base/color_scheme';
import {TrackData} from '../../components/tracks/track_data';
import {TimelineFetcher} from '../../components/tracks/track_helper';
import {checkerboardExcept} from '../../components/checkerboard';
import {TrackRenderer} from '../../public/track';
import {LONG, NUM, QueryResult} from '../../trace_processor/query_result';
import {uuidv4Sql} from '../../base/uuid';
import {
  TrackContext,
  TrackMouseEvent,
  TrackRenderContext,
} from '../../public/track';
import {Point2D} from '../../base/geom';
import {TimeScale} from '../../base/time_scale';
import {Trace} from '../../public/trace';
import {ThreadMap} from '../dev.perfetto.Thread/threads';
import {AsyncDisposableStack} from '../../base/disposable_stack';
import {
  createPerfettoTable,
  createVirtualTable,
} from '../../trace_processor/sql_utils';
import {Dataset} from '../../trace_processor/dataset';
import {TrackNode} from '../../public/workspace';
import {deferChunkedTask} from '../../base/chunked_task';

export const SLICE_TRACK_SUMMARY_KIND = 'SliceTrackSummary';

const MARGIN_TOP = 5;
const RECT_HEIGHT = 30;
const TRACK_HEIGHT = MARGIN_TOP * 2 + RECT_HEIGHT;

interface Data extends TrackData {
  maxLanes: number;

  // Slices are stored in a columnar fashion. All fields have the same length.
  counts: Uint32Array;
  starts: BigInt64Array;
  ends: BigInt64Array;
  utids: Int32Array;
  lanes: Uint32Array;
  // Cached color schemes for each slice (only used in 'sched' mode).
  colorSchemes: ColorScheme[];
  // Relative timestamps for fast rendering (relative to data.start)
  startRelNs: Float64Array;
  endRelNs: Float64Array;
}

export interface Config {
  pidForColor: bigint | number;
  upid: number | null;
  utid: number | null;
}

type Mode = 'sched' | 'slices';

interface GroupSummaryHover {
  utid: number;
  lane: number;
  count: number;
  pid?: bigint;
}

function computeHover(
  pos: Point2D | undefined,
  timescale: TimeScale,
  data: Data,
  threads: ThreadMap,
): GroupSummaryHover | undefined {
  if (pos === undefined) return undefined;

  const {x, y} = pos;
  if (y < MARGIN_TOP || y > MARGIN_TOP + RECT_HEIGHT) return undefined;

  const laneHeight = Math.floor(RECT_HEIGHT / data.maxLanes);
  const lane = Math.floor((y - MARGIN_TOP) / (laneHeight + 1));
  const t = timescale.pxToHpTime(x).toTime('floor');

  const [i, j] = searchRange(data.starts, t, searchEq(data.lanes, lane));
  if (i === j || i >= data.starts.length || t > data.ends[i]) return undefined;

  const utid = data.utids[i];
  const count = data.counts[i];
  const pid = threads.get(utid)?.pid;
  return {utid, lane, count, pid};
}

export class GroupSummaryTrack implements TrackRenderer {
  private hover?: GroupSummaryHover;
  private fetcher = new TimelineFetcher(this.onBoundsChange.bind(this));
  private trackUuid = uuidv4Sql();
  private mode: Mode = 'slices';
  private maxLanes: number = 1;
  private sliceTracks: Array<{uri: string; dataset: Dataset}> = [];

  // Cached color scheme for 'slices' mode (constant for track lifetime).
  private readonly slicesModeColor: ColorScheme;

  // Monitor for local hover state (triggers DOM redraw for tooltip).
  private readonly hoverMonitor = new Monitor([
    () => this.hover?.utid,
    () => this.hover?.lane,
    () => this.hover?.count,
  ]);

  constructor(
    private readonly trace: Trace,
    private readonly config: Config,
    private readonly cpuCount: number,
    private readonly threads: ThreadMap,
    hasSched: boolean,
  ) {
    this.mode = hasSched ? 'sched' : 'slices';
    this.slicesModeColor = colorForTid(this.config.pidForColor);
  }

  async onCreate(ctx: TrackContext): Promise<void> {
    if (this.mode === 'sched') {
      await this.createSchedMipmap();
    } else {
      await this.createSlicesMipmap(ctx.trackNode);
    }
  }

  private async createSchedMipmap(): Promise<void> {
    const getQuery = () => {
      if (this.config.upid !== null) {
        return `
          select
            s.id,
            s.ts,
            s.dur,
            c.cpu,
            s.utid
          from thread t
          cross join sched s using (utid)
          cross join cpu c using (ucpu)
          where
            t.is_idle = 0 and
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
          c.cpu,
          s.utid
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
      name: `process_summary_${this.trackUuid}`,
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

    this.maxLanes = this.cpuCount;
  }

  private fetchDatasetsFromSliceTracks(node: TrackNode) {
    assertTrue(
      this.mode === 'slices',
      'Can only collect slice tracks in slice mode',
    );
    const stack: TrackNode[] = [node];
    while (stack.length > 0 && this.sliceTracks.length < 8) {
      const node = stack.pop()!;

      // Try to get track and dataset
      const track =
        node.uri !== undefined
          ? this.trace.tracks.getTrack(node.uri)
          : undefined;
      const dataset = track?.renderer.getDataset?.();

      // Check if it's a valid slice track WITH depth column
      const sliceSchema = {ts: LONG, dur: LONG, depth: NUM};
      const isValidSliceTrack = dataset?.implements(sliceSchema) ?? false;

      if (isValidSliceTrack && dataset !== undefined) {
        // Add track - we'll filter to depth = 0 in SQL
        this.sliceTracks.push({
          uri: node.uri!,
          dataset: dataset,
        });
      } else {
        // Not valid - traverse children
        for (let i = node.children.length - 1; i >= 0; i--) {
          stack.push(node.children[i]);
        }
      }
    }
  }

  private async createSlicesMipmap(node: TrackNode): Promise<void> {
    // Fetch datasets from child tracks
    this.fetchDatasetsFromSliceTracks(node);

    if (this.sliceTracks.length === 0) {
      // No valid slice tracks found - create empty table
      await createVirtualTable({
        engine: this.trace.engine,
        name: `process_summary_${this.trackUuid}`,
        using: `__intrinsic_slice_mipmap((
          select
            cast(0 as int) as id,
            cast(0 as bigint) as ts,
            cast(0 as bigint) as dur,
            cast(0 as int) as depth
          where 0
        ))`,
      });
      this.maxLanes = 1;
      return;
    }

    // Create union of all slice tracks with track index as depth
    const unions = this.sliceTracks
      .map(({dataset}, idx) => {
        return `
        select
          id,
          ts,
          iif(dur = -1, trace_end() - ts, dur) as dur,
          ${idx} as depth
        from (${dataset.query()})
        where depth = 0
      `;
      })
      .join(' union all ');

    await createVirtualTable({
      engine: this.trace.engine,
      name: `process_summary_${this.trackUuid}`,
      using: `__intrinsic_slice_mipmap((
        ${unions}
      ))`,
    });
    this.maxLanes = 8;
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
      drop table process_summary_${this.trackUuid}
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

    const task = await deferChunkedTask();

    const numRows = queryRes.numRows();
    const slices: Data = {
      start,
      end,
      resolution,
      length: numRows,
      maxLanes: this.maxLanes,
      counts: new Uint32Array(numRows),
      starts: new BigInt64Array(numRows),
      ends: new BigInt64Array(numRows),
      lanes: new Uint32Array(numRows),
      utids: new Int32Array(numRows),
      colorSchemes: new Array(numRows),
      // Relative timestamps for fast rendering
      startRelNs: new Float64Array(numRows),
      endRelNs: new Float64Array(numRows),
    };

    const it = queryRes.iter({
      count: NUM,
      ts: LONG,
      dur: LONG,
      lane: NUM,
      utid: NUM,
    });

    for (let row = 0; it.valid(); it.next(), row++) {
      if (row % 50 === 0 && task.shouldYield()) {
        await task.yield();
      }

      const ts = it.ts;
      const dur = it.dur;
      const endTs = ts + dur;

      slices.counts[row] = it.count;
      slices.starts[row] = ts;
      slices.ends[row] = endTs;
      slices.lanes[row] = it.lane;
      slices.utids[row] = it.utid;
      slices.end = Time.max(Time.fromRaw(endTs), slices.end);

      // Store relative timestamps as floats for fast rendering
      slices.startRelNs[row] = Number(ts - start);
      slices.endRelNs[row] = Number(endTs - start);

      // Cache color scheme for 'sched' mode (depends on utid).
      if (this.mode === 'sched') {
        const threadInfo = this.threads.get(it.utid);
        slices.colorSchemes[row] = colorForThread(threadInfo);
      }
    }
    return slices;
  }

  private async queryData(
    start: time,
    end: time,
    bucketSize: duration,
  ): Promise<QueryResult> {
    if (this.mode === 'sched') {
      return this.trace.engine.query(`
        select
          (z.ts / ${bucketSize}) * ${bucketSize} as ts,
          iif(s.dur = -1, s.dur, max(z.dur, ${bucketSize})) as dur,
          z.count,
          z.depth as lane,
          s.utid
        from process_summary_${this.trackUuid}(
          ${start}, ${end}, ${bucketSize}
        ) z
        cross join sched s using (id)
      `);
    } else {
      return this.trace.engine.query(`
        select
          (z.ts / ${bucketSize}) * ${bucketSize} as ts,
          max(z.dur, ${bucketSize}) as dur,
          z.count,
          z.depth as lane,
          -1 as utid
        from process_summary_${this.trackUuid}(
          ${start}, ${end}, ${bucketSize}
        ) z
      `);
    }
  }

  getHeight(): number {
    return TRACK_HEIGHT;
  }

  renderTooltip(): m.Children {
    if (this.hover === undefined) {
      return undefined;
    }

    if (this.mode === 'sched') {
      // Show thread/process info for scheduling mode
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
    } else {
      // Show track name/info for slice mode
      const laneIndex = this.hover.lane;
      if (laneIndex < 0 || laneIndex >= this.sliceTracks.length) {
        return undefined;
      }

      const trackUri = this.sliceTracks[laneIndex].uri;
      const track = this.trace.tracks.getTrack(trackUri);
      const trackTitle = (track as {title?: string})?.title ?? trackUri;

      const count = this.hover.count;
      const countDiv = count > 1 && m('div', `${count} slices`);

      return m('.tooltip', [m('div', `Track: ${trackTitle}`), countDiv]);
    }
  }

  render({ctx, size, timescale}: TrackRenderContext): void {
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

    const laneHeight = Math.floor(RECT_HEIGHT / data.maxLanes);

    // Pre-compute conversion factors for fast timestamp-to-pixel conversion.
    const pxPerNs = timescale.durationToPx(1n);
    const baseOffsetPx = timescale.timeToPx(data.start);

    const timeline = this.trace.timeline;

    for (let i = 0; i < data.ends.length; i++) {
      // Use pre-computed relative timestamps for fast pixel conversion
      const rectStart = Math.floor(data.startRelNs[i] * pxPerNs + baseOffsetPx);
      const rectEnd = Math.floor(data.endRelNs[i] * pxPerNs + baseOffsetPx);

      // Cull slices that lie completely outside the visible window
      if (rectEnd < 0 || rectStart > size.width) continue;

      const utid = data.utids[i];
      const lane = data.lanes[i];

      const rectWidth = Math.max(1, rectEnd - rectStart);

      if (this.mode === 'sched') {
        // Scheduling mode: color by thread (utid) - use cached color scheme
        const colorScheme = data.colorSchemes[i];
        const threadInfo = this.threads.get(utid);
        // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
        const pid = (threadInfo ? threadInfo.pid : -1) || -1;

        const isHovering = timeline.hoveredUtid !== undefined;
        const isThreadHovered = timeline.hoveredUtid === utid;
        const isProcessHovered = timeline.hoveredPid === pid;

        if (isHovering && !isThreadHovered) {
          if (!isProcessHovered) {
            ctx.fillStyle = colorScheme.disabled.cssString;
          } else {
            ctx.fillStyle = colorScheme.variant.cssString;
          }
        } else {
          ctx.fillStyle = colorScheme.base.cssString;
        }
      } else {
        // Slice mode: consistent color based on pidForColor - use cached color
        ctx.fillStyle = this.slicesModeColor.base.cssString;
      }

      const y = MARGIN_TOP + laneHeight * lane + lane;
      ctx.fillRect(rectStart, y, rectWidth, laneHeight);
    }
  }

  onMouseMove({x, y, timescale}: TrackMouseEvent) {
    const data = this.fetcher.data;
    if (data === undefined) return;
    this.hover = computeHover({x, y}, timescale, data, this.threads);
    if (this.hoverMonitor.ifStateChanged()) {
      if (this.mode === 'sched') {
        this.trace.timeline.hoveredUtid = this.hover?.utid;
        this.trace.timeline.hoveredPid = this.hover?.pid;
      }
      this.trace.raf.scheduleFullRedraw();
    }
  }

  onMouseOut() {
    this.hover = undefined;
    if (this.hoverMonitor.ifStateChanged()) {
      if (this.mode === 'sched') {
        this.trace.timeline.hoveredUtid = undefined;
        this.trace.timeline.hoveredPid = undefined;
      }
      this.trace.raf.scheduleFullRedraw();
    }
  }
}
