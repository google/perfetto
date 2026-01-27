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
import {WebGLRenderer} from '../../base/webgl_renderer';
import {
  QuerySlot,
  SerialTaskQueue,
  QUERY_CANCELLED,
  CancellationSignal,
} from '../../base/query_slot';
import {raf} from '../../core/raf_scheduler';
import {deferToBackground, yieldBackgroundTask} from '../../base/utils';

export const SLICE_TRACK_SUMMARY_KIND = 'SliceTrackSummary';

const MARGIN_TOP = 5;
const RECT_HEIGHT = 30;
const TRACK_HEIGHT = MARGIN_TOP * 2 + RECT_HEIGHT;

interface Data {
  // Key fields for caching
  startNs: bigint;
  endNs: bigint;
  resolution: duration;

  // Data bounds (may differ from key if data extends beyond request)
  start: time;
  end: time;
  length: number;

  maxLanes: number;

  // Slices are stored in a columnar fashion. All fields have the same length.
  counts: Uint32Array;
  starts: BigInt64Array;
  ends: BigInt64Array;
  utids: Int32Array;
  lanes: Uint32Array;
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

interface TableInfo {
  maxLanes: number;
}

export class GroupSummaryTrack implements TrackRenderer {
  private hover?: GroupSummaryHover;
  private readonly taskQueue = new SerialTaskQueue();
  private readonly tableSlot = new QuerySlot<TableInfo>(this.taskQueue);
  private readonly dataSlot = new QuerySlot<Data>(this.taskQueue);
  private trackUuid = uuidv4Sql();
  private mode: Mode = 'slices';
  private sliceTracks: Array<{uri: string; dataset: Dataset}> = [];
  private trackNode?: TrackNode;

  // Current data for rendering and mouse events
  private currentData?: Data;

  // Track requested bounds to avoid re-triggering while query is in-flight
  private requestedBounds?: {
    startNs: bigint;
    endNs: bigint;
    resolution: duration;
  };

  // Cached colors per slice (computed once when data changes)
  private cachedColors?: Array<{
    base: {r: number; g: number; b: number; a: number};
    variant: {r: number; g: number; b: number; a: number};
    disabled: {r: number; g: number; b: number; a: number};
  }>;
  private cachedPids?: Array<bigint | undefined>;
  private lastCachedDataGeneration = -1;

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
  }

  async onCreate(ctx: TrackContext): Promise<void> {
    // Store trackNode for lazy table creation in render()
    this.trackNode = ctx.trackNode;
  }

  private async createSchedMipmap(): Promise<TableInfo> {
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

    return {maxLanes: this.cpuCount};
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

  private async createSlicesMipmap(node: TrackNode): Promise<TableInfo> {
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
      return {maxLanes: 1};
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
    return {maxLanes: 8};
  }

  async onDestroy(): Promise<void> {
    this.tableSlot.dispose();
    this.dataSlot.dispose();
    await this.trace.engine.tryQuery(`
      drop table process_summary_${this.trackUuid}
    `);
  }

  private async fetchData(
    start: time,
    end: time,
    resolution: duration,
    maxLanes: number,
    signal: CancellationSignal,
  ): Promise<Data | typeof QUERY_CANCELLED> {
    // Resolution must always be a power of 2 for this logic to work
    assertTrue(BIMath.popcount(resolution) === 1, `${resolution} not pow of 2`);

    const queryRes = await this.queryData(start, end, resolution);
    const numRows = queryRes.numRows();
    const slices: Data = {
      startNs: start,
      endNs: end,
      start,
      end,
      resolution,
      length: numRows,
      maxLanes,
      counts: new Uint32Array(numRows),
      starts: new BigInt64Array(numRows),
      ends: new BigInt64Array(numRows),
      lanes: new Uint32Array(numRows),
      utids: new Int32Array(numRows),
    };

    // Defer to idle time before iterating over results.
    let idle = await deferToBackground();

    const it = queryRes.iter({
      count: NUM,
      ts: LONG,
      dur: LONG,
      lane: NUM,
      utid: NUM,
    });

    // Iterate over results, yielding to idle callbacks when time runs out.
    // Check every 100 iterations to amortize the cost of timeRemaining().
    for (let row = 0; it.valid(); it.next(), row++) {
      if (row % 100 === 0) {
        // Check for cancellation
        if (signal.isCancelled) {
          console.log('Fetch data cancelled');
          return QUERY_CANCELLED;
        }
        if (idle.timeRemaining() <= 0) {
          idle = await yieldBackgroundTask();
        }
      }

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

    raf.scheduleCanvasRedraw();
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

  private cacheColors(data: Data): void {
    const numSlices = data.starts.length;
    this.cachedColors = new Array(numSlices);
    this.cachedPids = new Array(numSlices);

    for (let i = 0; i < numSlices; i++) {
      const utid = data.utids[i];
      const threadInfo = this.threads.get(utid);
      const colorScheme =
        this.mode === 'sched'
          ? colorForThread(threadInfo)
          : colorForTid(Number(this.config.pidForColor));

      this.cachedColors[i] = {
        base: colorScheme.base.rgba,
        variant: colorScheme.variant.rgba,
        disabled: colorScheme.disabled.rgba,
      };
      this.cachedPids[i] = threadInfo?.pid;
    }
  }

  private renderSlices(
    timescale: TimeScale,
    data: Data,
    canvasRenderer: WebGLRenderer,
  ): void {
    const laneHeight = Math.floor(RECT_HEIGHT / data.maxLanes);
    const hoveredUtid = this.trace.timeline.hoveredUtid;
    const hoveredPid = this.trace.timeline.hoveredPid;
    const isHovering = hoveredUtid !== undefined;

    for (let i = 0; i < data.starts.length; i++) {
      const tStart = Time.fromRaw(data.starts[i]);
      const tEnd = Time.fromRaw(data.ends[i]);
      const utid = data.utids[i];
      const lane = data.lanes[i];

      const rectStart = timescale.timeToPx(tStart);
      const rectEnd = timescale.timeToPx(tEnd);
      const rectWidth = Math.max(1, rectEnd - rectStart);

      const y = MARGIN_TOP + laneHeight * lane + lane;

      // Use cached colors
      const colors = this.cachedColors![i];
      let color;
      if (this.mode === 'sched' && isHovering) {
        const isThreadHovered = hoveredUtid === utid;
        const isProcessHovered =
          hoveredPid !== undefined && this.cachedPids![i] === hoveredPid;

        if (!isThreadHovered) {
          color = isProcessHovered ? colors.variant : colors.disabled;
        } else {
          color = colors.base;
        }
      } else {
        color = colors.base;
      }

      canvasRenderer.drawRect(rectStart, y, rectWidth, laneHeight, color);
    }
  }

  render({
    ctx,
    size,
    timescale,
    canvasRenderer,
    visibleWindow,
    resolution,
  }: TrackRenderContext): void {
    // First, ensure the mipmap table is created
    const tableResult = this.tableSlot.use({
      key: {mode: this.mode},
      queryFn: async () => {
        if (this.mode === 'sched') {
          return this.createSchedMipmap();
        } else {
          return this.createSlicesMipmap(assertExists(this.trackNode));
        }
      },
    });

    // Can't fetch data until table is ready
    if (!tableResult.data) return;

    const {maxLanes} = tableResult.data;

    // Check if requested bounds cover the viewport - only update bounds if
    // we moved outside the loaded bounds or resolution changed.
    const viewStart = visibleWindow.start.toTime();
    const viewEnd = visibleWindow.end.toTime();

    const bounds = this.requestedBounds;
    const needsNewData =
      bounds === undefined ||
      viewStart < bounds.startNs ||
      viewEnd > bounds.endNs ||
      resolution !== bounds.resolution;

    if (needsNewData) {
      // Compute padded bounds (one page on each side as "skirt")
      const viewDuration = viewEnd - viewStart;
      const paddedStart = BIMath.quantFloor(
        viewStart - viewDuration,
        resolution,
      );
      const paddedEnd = BIMath.quantCeil(viewEnd + viewDuration, resolution);

      // Track requested bounds to avoid re-triggering while in-flight
      this.requestedBounds = {
        startNs: paddedStart,
        endNs: paddedEnd,
        resolution,
      };
    }

    // Always call dataSlot.use() to get cached result or trigger fetch
    const {startNs, endNs, resolution: res} = this.requestedBounds!;
    const result = this.dataSlot.use({
      key: {startNs, endNs, resolution: res},
      queryFn: (signal) =>
        this.fetchData(
          Time.fromRaw(startNs),
          Time.fromRaw(endNs),
          res,
          maxLanes,
          signal,
        ),
    });

    // Update currentData when new data arrives
    if (result.data) {
      this.currentData = result.data;
    }

    const data = this.currentData;
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

    // Render slices using WebGL if available
    if (canvasRenderer !== undefined) {
      assertTrue(data.starts.length === data.ends.length);
      assertTrue(data.starts.length === data.utids.length);

      // Cache colors when data changes
      const dataGeneration = data.starts.length + Number(data.resolution);
      if (dataGeneration !== this.lastCachedDataGeneration) {
        this.cacheColors(data);
        this.lastCachedDataGeneration = dataGeneration;
      }

      this.renderSlices(timescale, data, canvasRenderer);
    }
  }

  onMouseMove({x, y, timescale}: TrackMouseEvent) {
    const data = this.currentData;
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
