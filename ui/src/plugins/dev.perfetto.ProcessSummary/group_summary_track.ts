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
import {
  colorForThread,
  colorForTid,
  USE_CONSISTENT_COLORS,
} from '../../components/colorizer';
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
}

// Pre-computed render data. Rebuilt when any rendering state changes.
// Ordered to minimize fillStyle changes during rendering.
interface RenderCache {
  readonly tStarts: BigInt64Array;
  readonly tEnds: BigInt64Array;
  readonly ys: Float64Array;
  readonly length: number;
  readonly laneHeight: number;
  // Sparse: only store where fillStyle changes.
  // runs[i] = {index, fillStyle} means "from index onwards, use fillStyle"
  readonly runs: ReadonlyArray<{
    readonly index: number;
    readonly fillStyle: string;
  }>;
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

  // Pre-computed render cache. Rebuilt when any rendering state changes.
  private renderCache?: RenderCache;
  private readonly renderMonitor: Monitor;

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
    this.renderMonitor = new Monitor([
      () => this.fetcher.data,
      () => USE_CONSISTENT_COLORS.get(),
      hasSched ? () => this.trace.timeline.hoveredUtid : () => undefined,
      hasSched ? () => this.trace.timeline.hoveredPid : () => undefined,
    ]);
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
    };

    const it = queryRes.iter({
      count: NUM,
      ts: LONG,
      dur: LONG,
      lane: NUM,
      utid: NUM,
    });

    for (let row = 0; it.valid(); it.next(), row++) {
      const start = Time.fromRaw(it.ts);
      const dur = it.dur;
      const end = Time.add(start, dur);

      slices.counts[row] = it.count;
      slices.starts[row] = start;
      slices.ends[row] = end;
      slices.lanes[row] = it.lane;
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

  render({ctx, size, timescale, visibleWindow}: TrackRenderContext): void {
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

    // Rebuild render cache if any rendering state changed (slow path).
    if (this.renderMonitor.ifStateChanged()) {
      this.renderCache = this.buildRenderCache(data);
    }

    const cache = this.renderCache;
    if (cache === undefined || cache.runs.length === 0) return;

    // Fast path: iterate with sparse fillStyle runs.
    const {tStarts, tEnds, ys, runs, laneHeight} = cache;
    let runIdx = 0;
    ctx.fillStyle = runs[0].fillStyle;

    for (let i = 0; i < cache.length; i++) {
      const tStart = Time.fromRaw(tStarts[i]);
      const tEnd = Time.fromRaw(tEnds[i]);

      // Cull slices that lie completely outside the visible window.
      if (!visibleWindow.overlaps(tStart, tEnd)) continue;

      // Advance to next fillStyle run if needed (integer comparison, not string).
      if (runIdx + 1 < runs.length && i >= runs[runIdx + 1].index) {
        ctx.fillStyle = runs[++runIdx].fillStyle;
      }

      const rectStart = Math.floor(timescale.timeToPx(tStart));
      const rectEnd = Math.floor(timescale.timeToPx(tEnd));
      ctx.fillRect(
        rectStart,
        ys[i],
        Math.max(1, rectEnd - rectStart),
        laneHeight,
      );
    }
  }

  private buildRenderCache(data: Data): RenderCache {
    const laneHeight = Math.floor(RECT_HEIGHT / data.maxLanes);
    const numRows = data.length;

    // Parallel arrays for render data.
    const tStarts = new BigInt64Array(numRows);
    const tEnds = new BigInt64Array(numRows);
    const ys = new Float64Array(numRows);
    const runs: Array<{index: number; fillStyle: string}> = [];

    if (this.mode === 'sched') {
      // Build color scheme cache per unique utid.
      const colorSchemes = new Map<number, ColorScheme>();
      for (let i = 0; i < numRows; i++) {
        const utid = data.utids[i];
        if (!colorSchemes.has(utid)) {
          const threadInfo = this.threads.get(utid);
          colorSchemes.set(utid, colorForThread(threadInfo));
        }
      }

      const hoveredUtid = this.trace.timeline.hoveredUtid;
      const hoveredPid = this.trace.timeline.hoveredPid;
      const isHovering = hoveredUtid !== undefined;

      // First pass: compute fillStyle for each element.
      const fillStyles: string[] = new Array(numRows);
      for (let i = 0; i < numRows; i++) {
        const utid = data.utids[i];
        const colorScheme = colorSchemes.get(utid)!;

        if (isHovering && hoveredUtid !== utid) {
          const threadInfo = this.threads.get(utid);
          const pid = (threadInfo ? threadInfo.pid : -1) ?? -1;
          if (hoveredPid !== pid) {
            fillStyles[i] = colorScheme.disabled.cssString;
          } else {
            fillStyles[i] = colorScheme.variant.cssString;
          }
        } else {
          fillStyles[i] = colorScheme.base.cssString;
        }
      }

      // Sort indices by fillStyle to minimize context switches.
      const sortedIndices = new Uint32Array(numRows);
      for (let i = 0; i < numRows; i++) {
        sortedIndices[i] = i;
      }
      sortedIndices.sort((a, b) => {
        const cmp = fillStyles[a].localeCompare(fillStyles[b]);
        return cmp !== 0 ? cmp : a - b; // Stable sort by original index
      });

      // Build sparse runs array and populate render data.
      let lastFillStyle = '';
      for (let si = 0; si < numRows; si++) {
        const i = sortedIndices[si];
        const lane = data.lanes[i];
        const fillStyle = fillStyles[i];

        if (fillStyle !== lastFillStyle) {
          runs.push({index: si, fillStyle});
          lastFillStyle = fillStyle;
        }

        tStarts[si] = data.starts[i];
        tEnds[si] = data.ends[i];
        ys[si] = MARGIN_TOP + laneHeight * lane + lane;
      }
    } else {
      // Slices mode: single color for all.
      const colorScheme = colorForTid(this.config.pidForColor);
      runs.push({index: 0, fillStyle: colorScheme.base.cssString});

      for (let i = 0; i < numRows; i++) {
        const lane = data.lanes[i];
        tStarts[i] = data.starts[i];
        tEnds[i] = data.ends[i];
        ys[i] = MARGIN_TOP + laneHeight * lane + lane;
      }
    }

    return {tStarts, tEnds, ys, length: numRows, laneHeight, runs};
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
