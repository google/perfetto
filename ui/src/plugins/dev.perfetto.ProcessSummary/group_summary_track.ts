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

import {Monitor} from '../../base/monitor';
import {assertExists, assertTrue} from '../../base/logging';
import {Time, time} from '../../base/time';
import m from 'mithril';
import {colorForThread, colorForTid} from '../../components/colorizer';
import {checkerboardExcept} from '../../components/checkerboard';
import {CacheKey} from '../../components/tracks/timeline_cache';
import {TrackRenderPipeline} from '../../components/tracks/track_render_pipeline';
import {OffscreenRenderer} from '../../components/tracks/offscreen_renderer';
import {TrackRenderer} from '../../public/track';
import {LONG, NUM, Row} from '../../trace_processor/query_result';
import {uuidv4Sql} from '../../base/uuid';
import {
  TrackContext,
  TrackMouseEvent,
  TrackRenderContext,
  TrackUpdateContext,
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

// Row spec for the group summary mipmap query.
const GROUP_SUMMARY_ROW = {
  count: NUM,
  ts: LONG,
  dur: LONG,
  lane: NUM,
  utid: NUM,
};
type GroupSummaryRow = typeof GROUP_SUMMARY_ROW;

// Cached color scheme for an entry.
interface CachedColorScheme {
  pid: bigint | undefined;
  base: string;
  disabled: string;
  variant: string;
}

// Entry stored in the pipeline buffer.
// Note: startTime/endTime/y/h match OffscreenRect interface for zero-allocation render.
interface GroupSummaryEntry {
  count: number;
  startTime: time;
  endTime: time;
  utid: number;
  lane: number;
  // Cached values for offscreen rendering and hover.
  y: number;
  h: number;
  pid: bigint | undefined;
  colorBase: string;
  colorDisabled: string;
  colorVariant: string;
}

// Global state tracked across entries.
interface GroupSummaryGlobalState {
  maxLanes: number;
  // Slices grouped by color for efficient batched rendering.
  byColor: Map<string, GroupSummaryEntry[]>;
  // Cache color schemes per utid to avoid recomputing colorForThread.
  colorCache: Map<number, CachedColorScheme>;
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
  data: GroupSummaryEntry[] | undefined,
  maxLanes: number,
  threads: ThreadMap,
): GroupSummaryHover | undefined {
  if (pos === undefined) return undefined;
  if (data === undefined || data.length === 0) return undefined;

  const {x, y} = pos;
  if (y < MARGIN_TOP || y > MARGIN_TOP + RECT_HEIGHT) return undefined;

  const laneHeight = Math.floor(RECT_HEIGHT / maxLanes);
  const lane = Math.floor((y - MARGIN_TOP) / (laneHeight + 1));
  const t = timescale.pxToHpTime(x).toTime('floor');

  // Find entry that matches the lane and contains the time
  for (const entry of data) {
    if (entry.lane === lane && t >= entry.startTime && t <= entry.endTime) {
      const pid = threads.get(entry.utid)?.pid;
      return {utid: entry.utid, lane: entry.lane, count: entry.count, pid};
    }
  }
  return undefined;
}

export class GroupSummaryTrack implements TrackRenderer {
  private hover?: GroupSummaryHover;
  private trackUuid = uuidv4Sql();
  private mode: Mode = 'slices';
  private maxLanes: number = 1;
  private sliceTracks: Array<{uri: string; dataset: Dataset}> = [];
  private cacheKey = CacheKey.zero();

  // Handles data loading with viewport caching, double-buffering, cooperative
  // multitasking, and abort detection when the viewport changes.
  private pipeline?: TrackRenderPipeline<
    Row & GroupSummaryRow,
    GroupSummaryEntry,
    GroupSummaryGlobalState
  >;

  // Offscreen renderer for pre-rendered slice fills.
  private readonly offscreenRenderer = new OffscreenRenderer();

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
    if (this.mode === 'sched') {
      await this.createSchedMipmap();
    } else {
      await this.createSlicesMipmap(ctx.trackNode);
    }
    this.initializePipeline();
  }

  private initializePipeline(): void {
    const getSql = (key: CacheKey) => {
      if (this.mode === 'sched') {
        return `
          select
            (z.ts / ${key.bucketSize}) * ${key.bucketSize} as ts,
            iif(s.dur = -1, s.dur, max(z.dur, ${key.bucketSize})) as dur,
            z.count,
            z.depth as lane,
            s.utid
          from process_summary_${this.trackUuid}(
            ${key.start}, ${key.end}, ${key.bucketSize}
          ) z
          cross join sched s using (id)
        `;
      } else {
        return `
          select
            (z.ts / ${key.bucketSize}) * ${key.bucketSize} as ts,
            max(z.dur, ${key.bucketSize}) as dur,
            z.count,
            z.depth as lane,
            -1 as utid
          from process_summary_${this.trackUuid}(
            ${key.start}, ${key.end}, ${key.bucketSize}
          ) z
        `;
      }
    };

    this.pipeline = new TrackRenderPipeline(
      this.trace,
      (_rawSql: string, key: CacheKey) => getSql(key),
      () => ({
        maxLanes: this.maxLanes,
        byColor: new Map<string, GroupSummaryEntry[]>(),
        colorCache: new Map<number, CachedColorScheme>(),
      }),
      (row, state) => {
        const startTime = Time.fromRaw(row.ts);
        const endTime = Time.add(startTime, row.dur);

        // Get cached color scheme or compute and cache it.
        let cached = state.colorCache.get(row.utid);
        if (cached === undefined) {
          if (this.mode === 'sched') {
            const threadInfo = this.threads.get(row.utid);
            const colorScheme = colorForThread(threadInfo);
            cached = {
              pid: threadInfo?.pid,
              base: colorScheme.base.cssString,
              disabled: colorScheme.disabled.cssString,
              variant: colorScheme.variant.cssString,
            };
          } else {
            const colorScheme = colorForTid(this.config.pidForColor);
            cached = {
              pid: undefined,
              base: colorScheme.base.cssString,
              disabled: colorScheme.disabled.cssString,
              variant: colorScheme.variant.cssString,
            };
          }
          state.colorCache.set(row.utid, cached);
        }

        // Pre-compute y/h for offscreen rendering to avoid allocation in render.
        const laneHeight = Math.floor(RECT_HEIGHT / this.maxLanes);
        const y = MARGIN_TOP + laneHeight * row.lane + row.lane;

        const entry: GroupSummaryEntry = {
          count: row.count,
          startTime,
          endTime,
          utid: row.utid,
          lane: row.lane,
          y,
          h: laneHeight,
          pid: cached.pid,
          colorBase: cached.base,
          colorDisabled: cached.disabled,
          colorVariant: cached.variant,
        };

        // Group by color for batched rendering.
        let group = state.byColor.get(cached.base);
        if (group === undefined) {
          group = [];
          state.byColor.set(cached.base, group);
        }
        group.push(entry);

        return entry;
      },
    );
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

  async onUpdate(ctx: TrackUpdateContext): Promise<void> {
    if (this.pipeline === undefined) return;

    const result = await this.pipeline.onUpdate('', GROUP_SUMMARY_ROW, ctx);
    if (result.status === 'updated') {
      this.cacheKey = this.pipeline.getCacheKey();
      const state = this.pipeline.getGlobalState();
      if (state !== undefined) {
        this.offscreenRenderer.render(
          state.byColor,
          this.cacheKey,
          TRACK_HEIGHT,
          // Use pre-computed y/h from entry to avoid allocations.
          (entry) => entry,
        );
      }
    }
  }

  async onDestroy(): Promise<void> {
    await this.trace.engine.tryQuery(`
      drop table process_summary_${this.trackUuid}
    `);
    this.offscreenRenderer.dispose();
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
    const data = this.pipeline?.getActiveBuffer() ?? [];

    if (data.length === 0) return; // Can't possibly draw anything.

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
      ctx,
      this.getHeight(),
      0,
      size.width,
      timescale.timeToPx(this.cacheKey.start),
      timescale.timeToPx(this.cacheKey.end),
    );

    const laneHeight = Math.floor(RECT_HEIGHT / this.maxLanes);
    const isHovering =
      this.mode === 'sched' && this.trace.timeline.hoveredUtid !== undefined;

    // Blit offscreen canvas for non-hover case (regular slice fills).
    // When hovering in sched mode, we need to redraw with modified colors.
    if (
      !isHovering &&
      this.offscreenRenderer.blit(ctx, timescale, this.cacheKey)
    ) {
      return; // No need for per-slice rendering when not hovering.
    }

    // Per-slice rendering: only needed when hovering in sched mode.
    for (const entry of data) {
      const tStart = entry.startTime;
      const tEnd = entry.endTime;

      // Cull slices that lie completely outside the visible window
      if (!visibleWindow.overlaps(tStart, tEnd)) continue;

      const rectStart = Math.floor(timescale.timeToPx(tStart));
      const rectEnd = Math.floor(timescale.timeToPx(tEnd));
      const rectWidth = Math.max(1, rectEnd - rectStart);

      // Use cached colors from entry.
      if (this.mode === 'sched') {
        const isThreadHovered = this.trace.timeline.hoveredUtid === entry.utid;
        const isProcessHovered = this.trace.timeline.hoveredPid === entry.pid;

        if (isHovering && !isThreadHovered) {
          if (!isProcessHovered) {
            ctx.fillStyle = entry.colorDisabled;
          } else {
            ctx.fillStyle = entry.colorVariant;
          }
        } else {
          ctx.fillStyle = entry.colorBase;
        }
      } else {
        ctx.fillStyle = entry.colorBase;
      }

      ctx.fillRect(rectStart, entry.y, rectWidth, laneHeight);
    }
  }

  onMouseMove({x, y, timescale}: TrackMouseEvent) {
    const data = this.pipeline?.getActiveBuffer();
    this.hover = computeHover(
      {x, y},
      timescale,
      data,
      this.maxLanes,
      this.threads,
    );
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
