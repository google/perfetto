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

import m from 'mithril';
import {BigintMath as BIMath} from '../../base/bigint_math';
import {searchEq, searchRange} from '../../base/binary_search';
import {deferChunkedTask} from '../../base/chunked_task';
import {Color} from '../../base/color';
import {ColorScheme} from '../../base/color_scheme';
import {AsyncDisposableStack} from '../../base/disposable_stack';
import {Point2D} from '../../base/geom';
import {assertExists, assertTrue} from '../../base/logging';
import {Monitor} from '../../base/monitor';
import {
  CancellationSignal,
  QUERY_CANCELLED,
  QuerySlot,
  SerialTaskQueue,
} from '../../base/query_slot';
import {duration, time, Time} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {checkerboardExcept} from '../../components/checkerboard';
import {colorForThread, colorForTid} from '../../components/colorizer';
import {Trace} from '../../public/trace';
import {
  TrackMouseEvent,
  TrackRenderContext,
  TrackRenderer,
} from '../../public/track';
import {TrackNode} from '../../public/workspace';
import {Dataset} from '../../trace_processor/dataset';
import {LONG, NUM} from '../../trace_processor/query_result';
import {
  createPerfettoTable,
  createVirtualTable,
} from '../../trace_processor/sql_utils';
import {ThreadMap} from '../dev.perfetto.Thread/threads';
import {CHUNKED_TASK_BACKGROUND_PRIORITY} from '../../components/tracks/feature_flags';
import {BufferedBounds} from '../../components/tracks/buffered_bounds';

export const SLICE_TRACK_SUMMARY_KIND = 'SliceTrackSummary';

const MARGIN_TOP = 5;
const RECT_HEIGHT = 30;
const TRACK_HEIGHT = MARGIN_TOP * 2 + RECT_HEIGHT;

interface Data {
  start: time;
  end: time;
  resolution: duration;
  length: number;
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
  startRelNs: Float32Array;
  durRelNs: Float32Array;
  // Pre-computed Y positions in screen pixels
  ys: Float32Array;
  // Working buffer for per-frame color computation (reused each frame)
  renderColors: Uint32Array;
  // Reusable patterns buffer (all zeros - no patterns)
  patterns: Uint8Array;
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

// Result from table creation query - contains the table and metadata
// Implements AsyncDisposable so QuerySlot can auto-dispose it
interface MipmapTable extends AsyncDisposable {
  tableName: string;
  maxLanes: number;
  sliceTracks: Array<{uri: string; dataset: Dataset}>;
}

export class GroupSummaryTrack implements TrackRenderer {
  private hover?: GroupSummaryHover;
  private readonly mode: Mode;
  private sliceTracks: Array<{uri: string; dataset: Dataset}> = [];

  // Cached color scheme for 'slices' mode (constant for track lifetime).
  private readonly slicesModeColor: ColorScheme;

  // Monitor for local hover state (triggers DOM redraw for tooltip).
  private readonly hoverMonitor = new Monitor([
    () => this.hover?.utid,
    () => this.hover?.lane,
    () => this.hover?.count,
  ]);

  // QuerySlot infrastructure
  private readonly queue = new SerialTaskQueue();
  private readonly tableSlot = new QuerySlot<MipmapTable>(this.queue);
  private readonly dataSlot = new QuerySlot<Data>(this.queue);

  // Cached data for rendering (populated from dataSlot)
  private data?: Data;

  // Track the bounds we've requested data for (with padding/skirt)
  // Only refetch when visible window exceeds these bounds
  private readonly bufferedBounds = new BufferedBounds();

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

  // Creates the mipmap table - called declaratively from render via QuerySlot
  private async createMipmapTable(trackNode: TrackNode): Promise<MipmapTable> {
    // Note: Table creation is typically fast, so we don't check cancellation here
    if (this.mode === 'sched') {
      return this.createSchedMipmap();
    } else {
      return this.createSlicesMipmap(trackNode);
    }
  }

  private async createSchedMipmap(): Promise<MipmapTable> {
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
    const tmpTable = await createPerfettoTable({
      engine: this.trace.engine,
      as: getQuery(),
    });
    trash.use(tmpTable);

    const mipmapTable = await createVirtualTable({
      engine: this.trace.engine,
      using: `__intrinsic_slice_mipmap((
        select
          s.id,
          s.ts,
          iif(
            s.dur = -1,
            ifnull(
              (
                select n.ts
                from ${tmpTable.name} n
                where n.ts > s.ts and n.cpu = s.cpu
                order by ts
                limit 1
              ),
              trace_end()
            ) - s.ts,
            s.dur
          ) as dur,
          s.cpu as depth
        from ${tmpTable.name} s
      ))`,
    });
    await trash.asyncDispose();

    return {
      tableName: mipmapTable.name,
      maxLanes: this.cpuCount,
      sliceTracks: [],
      [Symbol.asyncDispose]: () => mipmapTable[Symbol.asyncDispose](),
    };
  }

  private fetchDatasetsFromSliceTracks(
    trackNode: TrackNode,
  ): Array<{uri: string; dataset: Dataset}> {
    assertTrue(
      this.mode === 'slices',
      'Can only collect slice tracks in slice mode',
    );
    const sliceTracks: Array<{uri: string; dataset: Dataset}> = [];
    const stack: TrackNode[] = [trackNode];
    while (stack.length > 0 && sliceTracks.length < 8) {
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
        sliceTracks.push({
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
    return sliceTracks;
  }

  private async createSlicesMipmap(trackNode: TrackNode): Promise<MipmapTable> {
    // Fetch datasets from child tracks
    const sliceTracks = this.fetchDatasetsFromSliceTracks(trackNode);

    if (sliceTracks.length === 0) {
      // No valid slice tracks found - create empty table
      const table = await createVirtualTable({
        engine: this.trace.engine,
        using: `__intrinsic_slice_mipmap((
          select
            cast(0 as int) as id,
            cast(0 as bigint) as ts,
            cast(0 as bigint) as dur,
            cast(0 as int) as depth
          where 0
        ))`,
      });
      return {
        tableName: table.name,
        maxLanes: 1,
        sliceTracks: [],
        [Symbol.asyncDispose]: () => table[Symbol.asyncDispose](),
      };
    }

    // Create union of all slice tracks with track index as depth
    const unions = sliceTracks
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

    const table = await createVirtualTable({
      engine: this.trace.engine,
      using: `__intrinsic_slice_mipmap((
        ${unions}
      ))`,
    });
    return {
      tableName: table.name,
      maxLanes: 8,
      sliceTracks,
      [Symbol.asyncDispose]: () => table[Symbol.asyncDispose](),
    };
  }

  private async fetchData(
    tableName: string,
    maxLanes: number,
    start: time,
    end: time,
    resolution: duration,
    signal: CancellationSignal,
  ): Promise<Data> {
    // Resolution must always be a power of 2 for this logic to work
    assertTrue(BIMath.popcount(resolution) === 1, `${resolution} not pow of 2`);

    const queryRes = await this.queryData(tableName, start, end, resolution);

    // Check cancellation after query completes
    if (signal.isCancelled) throw QUERY_CANCELLED;

    const priority = CHUNKED_TASK_BACKGROUND_PRIORITY.get()
      ? 'background'
      : undefined;
    const task = await deferChunkedTask({priority});

    const numRows = queryRes.numRows();
    const laneHeight = Math.floor(RECT_HEIGHT / maxLanes);
    const slices: Data = {
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
      colorSchemes: new Array(numRows),
      // Relative timestamps for fast rendering
      startRelNs: new Float32Array(numRows),
      durRelNs: new Float32Array(numRows),
      // Pre-computed Y positions in screen pixels
      ys: new Float32Array(numRows),
      // Working buffer for per-frame color computation
      renderColors: new Uint32Array(numRows),
      // Reusable patterns buffer (all zeros - no patterns)
      patterns: new Uint8Array(numRows),
    };

    const it = queryRes.iter({
      count: NUM,
      ts: LONG,
      dur: LONG,
      lane: NUM,
      utid: NUM,
    });

    for (let row = 0; it.valid(); it.next(), row++) {
      // Periodically check for cancellation during iteration
      if (row % 50 === 0) {
        if (signal.isCancelled) {
          throw QUERY_CANCELLED;
        }

        if (task.shouldYield()) {
          await task.yield();
        }
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
      slices.durRelNs[row] = Number(dur);

      // Pre-compute Y position in screen pixels
      const lane = it.lane;
      slices.ys[row] = MARGIN_TOP + laneHeight * lane + lane;

      // Cache color scheme for 'sched' mode (depends on utid).
      if (this.mode === 'sched') {
        const threadInfo = this.threads.get(it.utid);
        slices.colorSchemes[row] = colorForThread(threadInfo);
      }
    }
    return slices;
  }

  private async queryData(
    tableName: string,
    start: time,
    end: time,
    bucketSize: duration,
  ) {
    if (this.mode === 'sched') {
      return this.trace.engine.query(`
        select
          (z.ts / ${bucketSize}) * ${bucketSize} as ts,
          iif(s.dur = -1, s.dur, max(z.dur, ${bucketSize})) as dur,
          z.count,
          z.depth as lane,
          s.utid
        from ${tableName}(
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
        from ${tableName}(
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

  render({
    ctx,
    size,
    timescale,
    renderer,
    visibleWindow,
    resolution,
    trackNode,
  }: TrackRenderContext): void {
    // Step 1: Declaratively ensure mipmap table exists
    const tableResult = this.tableSlot.use({
      // Key is constant - table only needs to be created once
      key: {mode: this.mode, upid: this.config.upid, utid: this.config.utid},
      queryFn: () => this.createMipmapTable(trackNode),
    });

    // Update sliceTracks from table result for tooltip rendering
    if (tableResult.data) {
      this.sliceTracks = tableResult.data.sliceTracks;
    }

    // Step 2: Declaratively fetch data from the table with buffered bounds
    const visibleSpan = visibleWindow.toTimeSpan();
    const bounds = this.bufferedBounds.update(visibleSpan, resolution);

    // Use the stable loaded bounds as the key - only changes when we decide to refetch
    const dataResult = this.dataSlot.use({
      key: {
        start: bounds.start,
        end: bounds.end,
        resolution: bounds.resolution,
      },
      queryFn: async (signal) => {
        const result = await this.trace.taskTracker.track(
          this.fetchData(
            tableResult.data!.tableName,
            tableResult.data!.maxLanes,
            bounds.start,
            bounds.end,
            bounds.resolution,
            signal,
          ),
          'Loading group summary',
        );
        this.trace.raf.scheduleCanvasRedraw();
        return result;
      },
      retainOn: ['start', 'end', 'resolution'], // Retain all old data until new data is loaded
      enabled: tableResult.data !== undefined,
    });

    // Cache data for mouse event handlers
    this.data = dataResult.data;

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

    assertTrue(data.starts.length === data.ends.length);
    assertTrue(data.starts.length === data.utids.length);

    const laneHeight = Math.floor(RECT_HEIGHT / data.maxLanes);
    const timeline = this.trace.timeline;
    const count = data.length;

    // Compute colors into the working buffer based on hover state
    const renderColors = data.renderColors;
    if (this.mode === 'sched') {
      const isHovering = timeline.hoveredUtid !== undefined;
      for (let i = 0; i < count; i++) {
        const colorScheme = data.colorSchemes[i];
        const utid = data.utids[i];
        const threadInfo = this.threads.get(utid);
        // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
        const pid = (threadInfo ? threadInfo.pid : -1) || -1;

        const isThreadHovered = timeline.hoveredUtid === utid;
        const isProcessHovered = timeline.hoveredPid === pid;

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
        renderColors[i] = color.rgba;
      }
    } else {
      // Slice mode: all same color
      const baseRgba = this.slicesModeColor.base.rgba;
      renderColors.fill(baseRgba);
    }

    // Draw all rects in one batch call
    // xs and ws are in data space (nanoseconds relative to data.start)
    // dataTransform converts: screenX = xs * scaleX + offsetX
    const pxPerNs = timescale.durationToPx(1n);
    const baseOffsetPx = timescale.timeToPx(data.start);

    renderer.drawRects(
      {
        xs: data.startRelNs,
        ys: data.ys,
        ws: data.durRelNs,
        h: laneHeight,
        colors: renderColors,
        patterns: data.patterns,
        count,
      },
      {offsetX: baseOffsetPx, offsetY: 0, scaleX: pxPerNs, scaleY: 1},
    );
  }

  onMouseMove({x, y, timescale}: TrackMouseEvent) {
    const data = this.data;
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
