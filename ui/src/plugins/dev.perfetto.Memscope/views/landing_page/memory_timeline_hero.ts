// Copyright (C) 2026 The Android Open Source Project
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

// Hero Interactive Memory Timeline: optional process RSS + Swap counter strip
// above three aligned horizontal sample tracks (Smaps, Java Heap, Native) on
// one shared time axis. Drag anywhere across the timeline to select a
// [base → sel] time range and switch every section below into Δ mode; click any
// marker to pin a snapshot or dump.

import m from 'mithril';
import {AsyncMemo} from '../../../../base/async_memo';
import {classNames} from '../../../../base/classnames';
import {Time, type time} from '../../../../base/time';
import {ProportionBar} from '../../../../components/widgets/charts/proportion_bar';
import type {Trace} from '../../../../public/trace';
import {LONG, NUM, STR} from '../../../../trace_processor/query_result';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {Icon} from '../../../../widgets/icon';
import type {HeapDump} from '../../../com.android.HeapDumpExplorer/types';
import {Inset} from '../../components/inset';
import {Panel} from '../../components/panel';
import {deltaColor, formatBytes, formatDelta} from './mem_format';
import {type MemSelection, nearestByTs} from './selection';
import {SMAPS_CATEGORIES, SMAPS_CATEGORY_CASE_SQL} from './smaps_categories';
import './memory_timeline_hero.scss';

export interface SmapsSnap {
  readonly ts: time;
  readonly total: number;
  readonly byCategory: ReadonlyMap<string, number>;
}

interface NativeSnap {
  readonly ts: time;
  readonly deltaBytes: number;
}

interface RssPoint {
  readonly ts: time;
  readonly totalRssSwap: number;
  readonly anonSwap?: number;
}

interface HeroTimelineData {
  readonly smaps: ReadonlyArray<SmapsSnap>;
  readonly nativeSnaps: ReadonlyArray<NativeSnap>;
  readonly javaHeapBytesByTs: ReadonlyMap<bigint, number>;
  readonly rssPoints: ReadonlyArray<RssPoint>;
  readonly rssPlotPoints: ReadonlyArray<RssPoint>;
  readonly rssMaxBytes: number;
  readonly hasAnonSplit: boolean;
}

const MAX_RSS_BUCKETS = 360;

// Preserves every spike and trough while capping SVG path vertices on dense
// ftrace rss_stat counter streams.
function downsampleRssPoints(
  points: ReadonlyArray<RssPoint>,
): ReadonlyArray<RssPoint> {
  if (points.length <= MAX_RSS_BUCKETS * 2) return points;
  const first = points[0];
  const last = points[points.length - 1];
  if (first === undefined || last === undefined) return points;

  const span = Number(last.ts - first.ts);
  if (span <= 0) return [first, last];

  const out: RssPoint[] = [first];
  let bucketIdx = -1;
  let minPt: RssPoint | undefined;
  let maxPt: RssPoint | undefined;

  const flush = () => {
    if (minPt === undefined || maxPt === undefined) return;
    if (minPt === maxPt) {
      out.push(minPt);
    } else if (minPt.ts <= maxPt.ts) {
      out.push(minPt, maxPt);
    } else {
      out.push(maxPt, minPt);
    }
    minPt = undefined;
    maxPt = undefined;
  };

  for (let i = 1; i < points.length - 1; i++) {
    const pt = points[i];
    if (pt === undefined) continue;
    const b = Math.min(
      MAX_RSS_BUCKETS - 1,
      Math.floor((Number(pt.ts - first.ts) / span) * MAX_RSS_BUCKETS),
    );
    if (b !== bucketIdx) {
      flush();
      bucketIdx = b;
      minPt = pt;
      maxPt = pt;
    } else {
      if (minPt === undefined || pt.totalRssSwap < minPt.totalRssSwap) {
        minPt = pt;
      }
      if (maxPt === undefined || pt.totalRssSwap > maxPt.totalRssSwap) {
        maxPt = pt;
      }
    }
  }
  flush();
  out.push(last);
  return out;
}

// Binary search for the latest step-counter point at or before `ts`.
function rssPointAtTs(
  points: ReadonlyArray<RssPoint>,
  ts: time,
): RssPoint | undefined {
  if (points.length === 0) return undefined;
  const first = points[0];
  if (first === undefined || ts < first.ts) return undefined;
  let lo = 0;
  let hi = points.length - 1;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    const pt = points[mid];
    if (pt !== undefined && pt.ts <= ts) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return points[lo];
}

async function loadRssCounterSeries(
  trace: Trace,
  upid: number,
): Promise<{
  readonly rssPoints: ReadonlyArray<RssPoint>;
  readonly rssPlotPoints: ReadonlyArray<RssPoint>;
  readonly rssMaxBytes: number;
  readonly hasAnonSplit: boolean;
}> {
  const rssPoints: RssPoint[] = [];
  let rssMaxBytes = 0;
  let hasAnonSplit = false;

  try {
    const res = await trace.engine.query(`
      SELECT
        c.ts AS ts,
        t.name AS name,
        CAST(c.value AS INT) AS val
      FROM counter c
      JOIN process_counter_track t ON c.track_id = t.id
      WHERE t.upid = ${upid}
        AND t.name IN (
          'mem.rss.anon',
          'mem.rss.file',
          'mem.rss.shmem',
          'mem.swap',
          'mem.rss'
        )
      ORDER BY c.ts ASC
    `);

    let anon: number | undefined;
    let file: number | undefined;
    let shmem: number | undefined;
    let swap: number | undefined;
    let totalRss: number | undefined;
    let curTs: bigint | undefined;

    const emit = (tsRaw: bigint) => {
      const hasSplit =
        anon !== undefined || file !== undefined || shmem !== undefined;
      const rss = hasSplit
        ? (anon ?? 0) + (file ?? 0) + (shmem ?? 0)
        : (totalRss ?? 0);
      const totalRssSwap = Math.max(0, rss + (swap ?? 0));
      const anonSwap =
        anon !== undefined ? Math.max(0, anon + (swap ?? 0)) : undefined;
      if (anonSwap !== undefined && anonSwap < totalRssSwap) {
        hasAnonSplit = true;
      }
      if (totalRssSwap > rssMaxBytes) rssMaxBytes = totalRssSwap;
      const prev = rssPoints[rssPoints.length - 1];
      if (
        prev !== undefined &&
        prev.totalRssSwap === totalRssSwap &&
        prev.anonSwap === anonSwap
      ) {
        return;
      }
      rssPoints.push({
        ts: Time.fromRaw(tsRaw),
        totalRssSwap,
        anonSwap,
      });
    };

    for (
      const it = res.iter({ts: LONG, name: STR, val: NUM});
      it.valid();
      it.next()
    ) {
      if (curTs !== undefined && it.ts !== curTs) {
        emit(curTs);
      }
      curTs = it.ts;
      const v = Math.max(0, it.val);
      switch (it.name) {
        case 'mem.rss.anon':
          anon = v;
          break;
        case 'mem.rss.file':
          file = v;
          break;
        case 'mem.rss.shmem':
          shmem = v;
          break;
        case 'mem.swap':
          swap = v;
          break;
        case 'mem.rss':
          totalRss = v;
          break;
      }
    }
    if (curTs !== undefined) {
      emit(curTs);
    }
  } catch {
    // Counter tracks absent or unavailable.
  }

  if (rssMaxBytes <= 0) {
    return {
      rssPoints: [],
      rssPlotPoints: [],
      rssMaxBytes: 0,
      hasAnonSplit: false,
    };
  }

  return {
    rssPoints,
    rssPlotPoints: downsampleRssPoints(rssPoints),
    rssMaxBytes,
    hasAnonSplit,
  };
}

async function loadHeroTimelineData(
  trace: Trace,
  upid: number,
): Promise<HeroTimelineData> {
  // 1. Smaps per-snapshot category breakdown
  const byTs = new Map<
    bigint,
    {ts: time; total: number; byCategory: Map<string, number>}
  >();
  const smapsRes = await trace.engine.query(`
    SELECT
      s.ts AS ts,
      ${SMAPS_CATEGORY_CASE_SQL} AS category,
      CAST(ifnull(SUM(s.rss_kb + s.swap_kb), 0) * 1024 AS INT) AS bytes
    FROM profiler_smaps s
    WHERE s.upid = ${upid}
    GROUP BY s.ts, category
    ORDER BY s.ts ASC
  `);
  for (
    const it = smapsRes.iter({ts: LONG, category: STR, bytes: NUM});
    it.valid();
    it.next()
  ) {
    let snap = byTs.get(it.ts);
    if (snap === undefined) {
      snap = {ts: Time.fromRaw(it.ts), total: 0, byCategory: new Map()};
      byTs.set(it.ts, snap);
    }
    snap.total += it.bytes;
    snap.byCategory.set(
      it.category,
      (snap.byCategory.get(it.category) ?? 0) + it.bytes,
    );
  }
  const smaps: SmapsSnap[] = Array.from(byTs.values());

  // 2. Native heapprofd sample timestamps + per-sample net delta bytes
  const nativeSnaps: NativeSnap[] = [];
  const nativeRes = await trace.engine.query(`
    SELECT
      a.ts AS ts,
      CAST(ifnull(SUM(a.size), 0) AS INT) AS delta_bytes
    FROM heap_profile_allocation a
    WHERE a.upid = ${upid}
    GROUP BY a.ts
    ORDER BY a.ts ASC
  `);
  for (
    const it = nativeRes.iter({ts: LONG, delta_bytes: NUM});
    it.valid();
    it.next()
  ) {
    nativeSnaps.push({ts: Time.fromRaw(it.ts), deltaBytes: it.delta_bytes});
  }

  // 3. Optional Java heap size per dump (for rich tooltips / hover HUD)
  const javaHeapBytesByTs = new Map<bigint, number>();
  try {
    await trace.engine.query(
      'INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_stats;',
    );
    const jRes = await trace.engine.query(`
      SELECT
        s.graph_sample_ts AS ts,
        CAST(
          s.reachable_heap_size + s.reachable_native_alloc_registry_size
          AS INT
        ) AS bytes
      FROM android_heap_graph_stats s
      WHERE s.upid = ${upid}
    `);
    for (const it = jRes.iter({ts: LONG, bytes: NUM}); it.valid(); it.next()) {
      javaHeapBytesByTs.set(it.ts, Math.max(0, it.bytes));
    }
  } catch {
    // Module unavailable or empty — pins still render on the Java track.
  }

  // 4. Optional process RSS + Swap counter series (drawn above the 3 tracks)
  const rss = await loadRssCounterSeries(trace, upid);

  return {
    smaps,
    nativeSnaps,
    javaHeapBytesByTs,
    ...rss,
  };
}

export interface MemoryTimelineHeroAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly procDumps: ReadonlyArray<HeapDump>;
  readonly activeDump: HeapDump | null;
  readonly selection?: MemSelection;
  readonly onSelect: (selection: MemSelection | undefined) => void;
  readonly onSelectDump?: (dump: HeapDump) => void;
}

const PAD_LEFT = 132;
const PAD_RIGHT = 20;
const CONTENT_TOP = 24;
const RSS_BAND_H = 74;
const RSS_BAND_GAP = 10;
const RSS_INNER_PAD_Y = 7;
const TRACK_H = 34;
const TRACK_GAP = 6;
const TRACK_COUNT = 3;
const DRAG_THRESHOLD_PX = 6;

type DragMode = 'range' | 'resize-base' | 'resize-sel';
type TrackKind = 'smaps' | 'java' | 'native';

function buildStepPaths(
  points: ReadonlyArray<{readonly x: number; readonly y: number}>,
  baseY: number,
  fallbackEndX: number,
): {readonly line: string; readonly area: string} {
  if (points.length === 0) return {line: '', area: ''};
  const first = points[0];
  if (first === undefined) return {line: '', area: ''};

  const lineParts: string[] = [`M ${first.x.toFixed(1)} ${first.y.toFixed(1)}`];
  let prevX = first.x;
  let prevY = first.y;
  for (let i = 1; i < points.length; i++) {
    const pt = points[i];
    if (pt === undefined) continue;
    lineParts.push(`H ${pt.x.toFixed(1)} V ${pt.y.toFixed(1)}`);
    prevX = pt.x;
    prevY = pt.y;
  }
  const endX =
    points.length === 1 || prevX - first.x < 2
      ? Math.max(prevX, fallbackEndX)
      : prevX;
  if (endX > prevX) {
    lineParts.push(`H ${endX.toFixed(1)}`);
  }
  const line = lineParts.join(' ');
  const area =
    `M ${first.x.toFixed(1)} ${baseY.toFixed(1)} ` +
    `L ${first.x.toFixed(1)} ${first.y.toFixed(1)} ` +
    lineParts.slice(1).join(' ') +
    ` L ${endX.toFixed(1)} ${baseY.toFixed(1)} Z`;
  void prevY;
  return {line, area};
}

export class MemoryTimelineHero implements m.ClassComponent<MemoryTimelineHeroAttrs> {
  private readonly slot = new AsyncMemo<HeroTimelineData>();
  private resizeObserver?: ResizeObserver;
  private observedWrap?: HTMLElement;
  private containerWidth = 960;

  private dragMode?: DragMode;
  private dragAnchorX = 0;
  private dragCurrentX = 0;
  private hoverX?: number;
  private hoverTrack?: TrackKind;

  private observeWrap(dom: Element) {
    const wrap = dom.querySelector('.pf-memtl__canvas-wrap');
    if (!(wrap instanceof HTMLElement)) return;
    if (this.observedWrap === wrap) return;
    this.resizeObserver?.disconnect();
    this.observedWrap = wrap;
    const update = () => {
      const w = Math.max(360, Math.floor(wrap.clientWidth));
      if (w !== this.containerWidth) {
        this.containerWidth = w;
        m.redraw();
      }
    };
    update();
    this.resizeObserver = new ResizeObserver(update);
    this.resizeObserver.observe(wrap);
  }

  oncreate({dom}: m.VnodeDOM<MemoryTimelineHeroAttrs>) {
    this.observeWrap(dom);
  }

  onupdate({dom}: m.VnodeDOM<MemoryTimelineHeroAttrs>) {
    this.observeWrap(dom);
  }

  onremove() {
    this.resizeObserver?.disconnect();
    this.resizeObserver = undefined;
    this.observedWrap = undefined;
    this.slot.dispose();
  }

  view({attrs}: m.Vnode<MemoryTimelineHeroAttrs>): m.Children {
    const {
      trace,
      upid,
      procDumps,
      activeDump,
      selection,
      onSelect,
      onSelectDump,
    } = attrs;

    const data = this.slot.use({
      key: {traceId: trace.traceInfo.uuid, upid},
      compute: () => loadHeroTimelineData(trace, upid),
    }).data;

    const smaps = data?.smaps ?? [];
    const nativeSnaps = data?.nativeSnaps ?? [];
    const javaHeapBytesByTs =
      data?.javaHeapBytesByTs ?? new Map<bigint, number>();
    const rssPoints = data?.rssPoints ?? [];
    const rssPlotPoints = data?.rssPlotPoints ?? [];
    const rssMaxBytes = data?.rssMaxBytes ?? 0;
    const hasAnonSplit = data?.hasAnonSplit ?? false;
    const hasRss = rssPlotPoints.length > 0 && rssMaxBytes > 0;

    // Dynamic vertical layout: include RSS + Swap counter strip above the 3
    // sample tracks whenever process RSS counters exist in the trace.
    const rssTop = CONTENT_TOP;
    const rssBottom = rssTop + RSS_BAND_H;
    const tracksTop = hasRss ? rssBottom + RSS_BAND_GAP : CONTENT_TOP;
    const tracksBottom =
      tracksTop + TRACK_COUNT * TRACK_H + (TRACK_COUNT - 1) * TRACK_GAP;
    const axisY = tracksBottom + 16;
    const svgHeight = axisY + 10;

    const trackTopY = (idx: number): number =>
      tracksTop + idx * (TRACK_H + TRACK_GAP);
    const trackCenterY = (idx: number): number => trackTopY(idx) + TRACK_H / 2;

    const traceStart = trace.traceInfo.start;
    const traceEnd = trace.traceInfo.end;
    const spanNs = Math.max(1, Number(traceEnd - traceStart));
    const durationS = spanNs / 1e9;

    const svgW = this.containerWidth;
    const plotW = Math.max(120, svgW - PAD_LEFT - PAD_RIGHT);

    const xOfTs = (ts: time): number => {
      const frac = Math.min(1, Math.max(0, Number(ts - traceStart) / spanNs));
      return PAD_LEFT + frac * plotW;
    };

    const tsOfX = (x: number): time => {
      const clamped = Math.min(PAD_LEFT + plotW, Math.max(PAD_LEFT, x));
      const frac = (clamped - PAD_LEFT) / plotW;
      return Time.fromRaw(traceStart + BigInt(Math.round(frac * spanNs)));
    };

    const rssPlotTop = rssTop + RSS_INNER_PAD_Y;
    const rssPlotBottom = rssBottom - RSS_INNER_PAD_Y;
    const rssPlotH = Math.max(1, rssPlotBottom - rssPlotTop);
    const yOfRss = (bytes: number): number => {
      const frac = Math.min(1, Math.max(0, bytes / Math.max(1, rssMaxBytes)));
      return rssPlotBottom - frac * rssPlotH;
    };

    const secOf = (ts: time): string =>
      `${(Number(ts - traceStart) / 1e9).toFixed(1)}s`;

    const trackAtY = (y: number): TrackKind | undefined => {
      if (y >= trackTopY(0) && y <= trackTopY(0) + TRACK_H) return 'smaps';
      if (y >= trackTopY(1) && y <= trackTopY(1) + TRACK_H) return 'java';
      if (y >= trackTopY(2) && y <= trackTopY(2) + TRACK_H) return 'native';
      return undefined;
    };

    // Active range or live drag preview coordinates.
    const isDragging = this.dragMode !== undefined;
    const isDragRange =
      isDragging &&
      Math.abs(this.dragCurrentX - this.dragAnchorX) >= DRAG_THRESHOLD_PX;
    const liveRange = isDragRange
      ? {
          x1: Math.min(this.dragAnchorX, this.dragCurrentX),
          x2: Math.max(this.dragAnchorX, this.dragCurrentX),
        }
      : selection?.base !== undefined
        ? {
            x1: Math.min(xOfTs(selection.base), xOfTs(selection.sel)),
            x2: Math.max(xOfTs(selection.base), xOfTs(selection.sel)),
          }
        : undefined;

    const liveBaseTs =
      isDragRange && liveRange !== undefined
        ? tsOfX(liveRange.x1)
        : selection?.base;
    const liveSelTs =
      isDragRange && liveRange !== undefined
        ? tsOfX(liveRange.x2)
        : selection?.sel;

    const liveBaseSnap =
      liveBaseTs !== undefined && smaps.length > 0
        ? nearestByTs(smaps, liveBaseTs)
        : undefined;
    const liveSelSnap =
      liveSelTs !== undefined && smaps.length > 0
        ? nearestByTs(smaps, liveSelTs)
        : smaps[smaps.length - 1];

    const hasSmapsRangeDelta =
      liveBaseSnap !== undefined &&
      liveSelSnap !== undefined &&
      liveBaseSnap.ts !== liveSelSnap.ts;
    const liveSmapsDelta = hasSmapsRangeDelta
      ? liveSelSnap.total - liveBaseSnap.total
      : undefined;

    const liveBaseRss =
      liveBaseTs !== undefined
        ? rssPointAtTs(rssPoints, liveBaseTs)
        : undefined;
    const liveSelRss =
      liveSelTs !== undefined
        ? rssPointAtTs(rssPoints, liveSelTs)
        : rssPoints[rssPoints.length - 1];
    const liveRssDelta =
      liveBaseRss !== undefined && liveSelRss !== undefined
        ? liveSelRss.totalRssSwap - liveBaseRss.totalRssSwap
        : undefined;
    const liveHeaderDelta = liveSmapsDelta ?? liveRssDelta;

    // Precompute SVG paths for RSS + Swap counter curves.
    const rssTotalPaths = hasRss
      ? buildStepPaths(
          rssPlotPoints.map((p) => ({
            x: xOfTs(p.ts),
            y: yOfRss(p.totalRssSwap),
          })),
          rssPlotBottom,
          PAD_LEFT + plotW,
        )
      : undefined;
    const rssAnonPaths =
      hasRss && hasAnonSplit
        ? buildStepPaths(
            rssPlotPoints
              .filter((p) => p.anonSwap !== undefined)
              .map((p) => ({
                x: xOfTs(p.ts),
                y: yOfRss(p.anonSwap ?? 0),
              })),
            rssPlotBottom,
            PAD_LEFT + plotW,
          )
        : undefined;

    // X-axis ticks (5–8 clean intervals).
    const xTickCount = Math.max(4, Math.min(8, Math.floor(plotW / 120)));
    const xTicks: {x: number; label: string}[] = [];
    for (let i = 0; i <= xTickCount; i++) {
      const frac = i / xTickCount;
      xTicks.push({
        x: PAD_LEFT + frac * plotW,
        label: `${(frac * durationS).toFixed(durationS < 10 ? 1 : 0)}s`,
      });
    }

    const localCoords = (
      e: PointerEvent,
    ): {x: number; rawX: number; y: number} => {
      const svg = e.currentTarget as SVGSVGElement;
      const rect = svg.getBoundingClientRect();
      const rawX = e.clientX - rect.left;
      return {
        x: Math.min(PAD_LEFT + plotW, Math.max(PAD_LEFT, rawX)),
        rawX,
        y: e.clientY - rect.top,
      };
    };

    const startDrag = (mode: DragMode, anchorX: number, e: PointerEvent) => {
      if (e.button !== 0) return;
      e.stopPropagation();
      const svg =
        e.currentTarget instanceof SVGSVGElement
          ? e.currentTarget
          : (e.currentTarget as Element).closest('svg');
      if (svg instanceof SVGSVGElement) {
        svg.setPointerCapture(e.pointerId);
        const rect = svg.getBoundingClientRect();
        const curX = Math.min(
          PAD_LEFT + plotW,
          Math.max(PAD_LEFT, e.clientX - rect.left),
        );
        this.dragMode = mode;
        this.dragAnchorX = anchorX;
        this.dragCurrentX = curX;
        this.hoverX = undefined;
        this.hoverTrack = undefined;
      }
    };

    const onPointerDown = (e: PointerEvent) => {
      const {x, rawX} = localCoords(e);
      if (rawX < PAD_LEFT) return;
      startDrag('range', x, e);
    };

    const onPointerMove = (e: PointerEvent) => {
      const {x, rawX, y} = localCoords(e);
      if (this.dragMode !== undefined) {
        this.dragCurrentX = x;
      } else if (rawX < PAD_LEFT) {
        this.hoverX = undefined;
        this.hoverTrack = undefined;
      } else {
        this.hoverX = x;
        this.hoverTrack = trackAtY(y);
      }
    };

    const onPointerUp = (e: PointerEvent) => {
      if (this.dragMode === undefined) return;
      const svg = e.currentTarget as SVGSVGElement;
      if (svg.hasPointerCapture(e.pointerId)) {
        svg.releasePointerCapture(e.pointerId);
      }
      const mode = this.dragMode;
      const x1 = Math.min(this.dragAnchorX, this.dragCurrentX);
      const x2 = Math.max(this.dragAnchorX, this.dragCurrentX);
      const {y} = localCoords(e);
      this.dragMode = undefined;

      if (mode !== 'range' || x2 - x1 >= DRAG_THRESHOLD_PX) {
        const tBase = tsOfX(x1);
        const tSel = tsOfX(x2);
        onSelect({base: tBase, sel: tSel});
        if (procDumps.length > 0 && onSelectDump !== undefined) {
          const nearestDump = nearestByTs(procDumps, tSel);
          if (nearestDump !== undefined) onSelectDump(nearestDump);
        }
        return;
      }

      // Click: snap to the nearest marker on the clicked track first, then fall
      // back to any nearby marker across all tracks.
      const clickX = this.dragCurrentX;
      const clickedTrack = trackAtY(y);
      const clickTs = tsOfX(clickX);

      if (clickedTrack === 'java' && procDumps.length > 0) {
        const d = nearestByTs(procDumps, clickTs);
        if (d !== undefined) {
          onSelect({sel: d.ts});
          onSelectDump?.(d);
          return;
        }
      }
      if (clickedTrack === 'smaps' && smaps.length > 0) {
        const s = nearestByTs(smaps, clickTs);
        if (s !== undefined) {
          onSelect({sel: s.ts});
          if (procDumps.length > 0 && onSelectDump !== undefined) {
            const d = nearestByTs(procDumps, s.ts);
            if (d !== undefined) onSelectDump(d);
          }
          return;
        }
      }
      if (clickedTrack === 'native' && nativeSnaps.length > 0) {
        const n = nearestByTs(nativeSnaps, clickTs);
        if (n !== undefined) {
          onSelect({sel: n.ts});
          return;
        }
      }

      // Fallback: nearest sample across any track
      const candidates: {ts: time; dump?: HeapDump}[] = [
        ...smaps.map((s) => ({ts: s.ts})),
        ...procDumps.map((d) => ({ts: d.ts, dump: d})),
        ...nativeSnaps.map((n) => ({ts: n.ts})),
      ];
      const nearest = nearestByTs(candidates, clickTs);
      const targetTs = nearest?.ts ?? clickTs;
      onSelect({sel: targetTs});
      if (nearest?.dump !== undefined) {
        onSelectDump?.(nearest.dump);
      } else if (procDumps.length > 0 && onSelectDump !== undefined) {
        const d = nearestByTs(procDumps, targetTs);
        if (d !== undefined) onSelectDump(d);
      }
    };

    const onPointerLeave = () => {
      if (this.dragMode === undefined) {
        this.hoverX = undefined;
        this.hoverTrack = undefined;
      }
    };

    // Hover HUD readout
    const hoverX = this.hoverX;
    const hoverTs = hoverX !== undefined ? tsOfX(hoverX) : undefined;
    const hoverRss =
      hoverTs !== undefined ? rssPointAtTs(rssPoints, hoverTs) : undefined;
    const pickHover = <T extends {readonly ts: time}>(
      items: ReadonlyArray<T>,
      track: TrackKind,
      radiusPx: number,
    ): T | undefined => {
      if (hoverX === undefined || hoverTs === undefined || items.length === 0) {
        return undefined;
      }
      const nearest = nearestByTs(items, hoverTs);
      if (nearest === undefined) return undefined;
      if (
        this.hoverTrack === track ||
        Math.abs(xOfTs(nearest.ts) - hoverX) <= radiusPx
      ) {
        return nearest;
      }
      return undefined;
    };
    const hoverSnap = pickHover(smaps, 'smaps', 12);
    const hoverDump = pickHover(procDumps, 'java', 14);
    const hoverNative = pickHover(nativeSnaps, 'native', 10);
    const hoverDumpBytes =
      hoverDump !== undefined ? javaHeapBytesByTs.get(hoverDump.ts) : undefined;

    const firstSmap = smaps[0];
    const lastSmap = smaps[smaps.length - 1];
    const firstDump = procDumps[0];
    const lastDump = procDumps[procDumps.length - 1];
    const firstNative = nativeSnaps[0];
    const lastNative = nativeSnaps[nativeSnaps.length - 1];
    const firstRss = rssPoints[0];
    const lastRss = rssPoints[rssPoints.length - 1];
    const canCompareFullTrace =
      (smaps.length >= 2 &&
        firstSmap !== undefined &&
        lastSmap !== undefined) ||
      (procDumps.length >= 2 &&
        firstDump !== undefined &&
        lastDump !== undefined) ||
      (nativeSnaps.length >= 2 &&
        firstNative !== undefined &&
        lastNative !== undefined) ||
      (rssPoints.length >= 2 &&
        firstRss !== undefined &&
        lastRss !== undefined);

    const compareFullTrace = () => {
      if (
        smaps.length >= 2 &&
        firstSmap !== undefined &&
        lastSmap !== undefined
      ) {
        onSelect({base: firstSmap.ts, sel: lastSmap.ts});
      } else if (
        procDumps.length >= 2 &&
        firstDump !== undefined &&
        lastDump !== undefined
      ) {
        onSelect({base: firstDump.ts, sel: lastDump.ts});
        onSelectDump?.(lastDump);
      } else if (
        nativeSnaps.length >= 2 &&
        firstNative !== undefined &&
        lastNative !== undefined
      ) {
        onSelect({base: firstNative.ts, sel: lastNative.ts});
      } else if (
        rssPoints.length >= 2 &&
        firstRss !== undefined &&
        lastRss !== undefined
      ) {
        onSelect({base: firstRss.ts, sel: lastRss.ts});
      }
    };

    const isComparing = selection?.base !== undefined;
    const headerStatus = this.renderHeaderStatus({
      isComparing,
      selection,
      liveDelta: liveHeaderDelta,
      liveSelSnap,
      liveSelRss,
      secOf,
      canCompareFullTrace,
      onCompareFullTrace: compareFullTrace,
      onClear: () => onSelect(undefined),
    });

    const tracks: ReadonlyArray<{
      readonly id: TrackKind;
      readonly label: string;
      readonly count: number;
    }> = [
      {id: 'smaps', label: 'Smaps', count: smaps.length},
      {id: 'java', label: 'Java Heap', count: procDumps.length},
      {id: 'native', label: 'Native', count: nativeSnaps.length},
    ];

    const isInRange = (ts: time): boolean => {
      if (liveBaseTs === undefined || liveSelTs === undefined) return false;
      const lo = liveBaseTs < liveSelTs ? liveBaseTs : liveSelTs;
      const hi = liveBaseTs < liveSelTs ? liveSelTs : liveBaseTs;
      return ts >= lo && ts <= hi;
    };

    const gradId = `pf-memtl-rss-grad-${upid}`;
    const anonGradId = `pf-memtl-anon-grad-${upid}`;

    return m(
      Panel,
      {className: 'pf-memtl-hero'},
      m(Panel.Header, {
        title: 'Memory Samples Timeline',
        subtitle:
          'Drag across the timeline to select a time range and view all ' +
          'sections below as a delta (Δ). Click any marker to pin.',
        controls: headerStatus,
      }),
      m(Panel.Body, {className: 'pf-memtl-hero__body'}, [
        data === undefined
          ? m('.pf-memscope-placeholder', 'Loading memory samples…')
          : m('.pf-memtl__canvas-wrap', [
              m(
                'svg.pf-memtl__svg',
                {
                  width: svgW,
                  height: svgHeight,
                  viewBox: `0 0 ${svgW} ${svgHeight}`,
                  style: {height: `${svgHeight}px`},
                  onpointerdown: onPointerDown,
                  onpointermove: onPointerMove,
                  onpointerup: onPointerUp,
                  onpointerleave: onPointerLeave,
                  ondblclick: () => onSelect(undefined),
                },
                [
                  // Gradient definitions for optional RSS + Swap counter strip
                  hasRss &&
                    m('defs', [
                      m(
                        'linearGradient',
                        {id: gradId, x1: '0', y1: '0', x2: '0', y2: '1'},
                        [
                          m('stop', {
                            'offset': '0%',
                            'stop-color': '#7c4dff',
                            'stop-opacity': '0.22',
                          }),
                          m('stop', {
                            'offset': '100%',
                            'stop-color': '#7c4dff',
                            'stop-opacity': '0.02',
                          }),
                        ],
                      ),
                      hasAnonSplit &&
                        m(
                          'linearGradient',
                          {id: anonGradId, x1: '0', y1: '0', x2: '0', y2: '1'},
                          [
                            m('stop', {
                              'offset': '0%',
                              'stop-color': '#5c6bc0',
                              'stop-opacity': '0.24',
                            }),
                            m('stop', {
                              'offset': '100%',
                              'stop-color': '#5c6bc0',
                              'stop-opacity': '0.04',
                            }),
                          ],
                        ),
                    ]),

                  // Optional Process RSS + Swap Counter Track (above sample tracks)
                  hasRss &&
                    rssTotalPaths !== undefined &&
                    m('g.pf-memtl__rss-band', [
                      m('rect.pf-memtl__track-bg.pf-memtl__rss-bg', {
                        x: PAD_LEFT,
                        y: rssTop,
                        width: plotW,
                        height: RSS_BAND_H,
                        rx: 6,
                      }),
                      // Subtle 50% horizontal guide line
                      m('line.pf-memtl__rss-hgrid', {
                        x1: PAD_LEFT + 1,
                        y1: (rssPlotTop + rssPlotBottom) / 2,
                        x2: PAD_LEFT + plotW - 1,
                        y2: (rssPlotTop + rssPlotBottom) / 2,
                      }),
                      // Left header: swatch + title + optional Anon+Swap legend
                      m('rect.pf-memtl__rss-swatch', {
                        x: 7,
                        y: rssTop + 16,
                        width: 10,
                        height: 3,
                        rx: 1.5,
                      }),
                      m(
                        'text.pf-memtl__track-name',
                        {x: 22, y: rssTop + 21},
                        'RSS + Swap',
                      ),
                      hasAnonSplit &&
                        m('g.pf-memtl__rss-sublegend', [
                          m('rect.pf-memtl__rss-swatch--anon', {
                            x: 7,
                            y: rssTop + 36,
                            width: 10,
                            height: 3,
                            rx: 1.5,
                          }),
                          m(
                            'text.pf-memtl__rss-sublabel',
                            {x: 22, y: rssTop + 41},
                            'Anon + Swap',
                          ),
                        ]),
                      // Filled area + step line for Total RSS + Swap
                      m('path.pf-memtl__rss-area', {
                        d: rssTotalPaths.area,
                        fill: `url(#${gradId})`,
                      }),
                      // Optional inner filled area + line for Anon + Swap
                      rssAnonPaths !== undefined &&
                        rssAnonPaths.line !== '' &&
                        m('g', [
                          m('path.pf-memtl__rss-area', {
                            d: rssAnonPaths.area,
                            fill: `url(#${anonGradId})`,
                          }),
                          m('path.pf-memtl__rss-line--anon', {
                            d: rssAnonPaths.line,
                          }),
                        ]),
                      m('path.pf-memtl__rss-line', {
                        d: rssTotalPaths.line,
                      }),
                      // Y-axis max / 0 labels pinned cleanly inside the plot's left edge
                      m(
                        'text.pf-memtl__rss-ytick',
                        {x: PAD_LEFT + 6, y: rssTop + 13},
                        formatBytes(rssMaxBytes),
                      ),
                      m(
                        'text.pf-memtl__rss-ytick',
                        {x: PAD_LEFT + 6, y: rssBottom - 4},
                        '0',
                      ),
                    ]),

                  // Track lanes & left track headers
                  tracks.map((trk, idx) => {
                    const topY = trackTopY(idx);
                    const midY = trackCenterY(idx);
                    const isEmpty = trk.count === 0;
                    const isHoveredTrack = this.hoverTrack === trk.id;
                    return m(
                      'g.pf-memtl__track',
                      {
                        key: trk.id,
                        class: classNames(
                          isEmpty && 'pf-memtl__track--empty',
                          isHoveredTrack && 'pf-memtl__track--hovered',
                        ),
                      },
                      [
                        // Track band background
                        m('rect.pf-memtl__track-bg', {
                          x: PAD_LEFT,
                          y: topY,
                          width: plotW,
                          height: TRACK_H,
                          rx: 6,
                        }),
                        // Center guide rail inside track
                        m('line.pf-memtl__track-rail', {
                          x1: PAD_LEFT + 8,
                          y1: midY,
                          x2: PAD_LEFT + plotW - 8,
                          y2: midY,
                        }),
                        // Left track badge shape matching its markers
                        trk.id === 'smaps' &&
                          m(
                            'circle.pf-memtl__badge-dot.pf-memtl__badge-dot--smaps',
                            {
                              cx: 12,
                              cy: midY,
                              r: 4.5,
                            },
                          ),
                        trk.id === 'java' &&
                          m(
                            'polygon.pf-memtl__badge-dot.pf-memtl__badge-dot--java',
                            {
                              points: `12,${midY - 5.5} 17.5,${midY} 12,${midY + 5.5} 6.5,${midY}`,
                            },
                          ),
                        trk.id === 'native' &&
                          m(
                            'rect.pf-memtl__badge-dot.pf-memtl__badge-dot--native',
                            {
                              x: 9.5,
                              y: midY - 6,
                              width: 5,
                              height: 12,
                              rx: 2.5,
                            },
                          ),
                        // Track name + count
                        m(
                          'text.pf-memtl__track-name',
                          {x: 24, y: midY + 4},
                          trk.label,
                        ),
                        m(
                          'text.pf-memtl__track-count',
                          {x: PAD_LEFT - 10, y: midY + 4},
                          `${trk.count}`,
                        ),
                        isEmpty &&
                          m(
                            'text.pf-memtl__track-empty-msg',
                            {x: PAD_LEFT + plotW / 2, y: midY + 4},
                            'No samples captured',
                          ),
                      ],
                    );
                  }),

                  // Subtle vertical time gridlines spanning RSS band + tracks
                  xTicks.map((t, i) =>
                    m('line.pf-memtl__vgrid', {
                      key: `vg-${i}`,
                      x1: t.x,
                      y1: CONTENT_TOP,
                      x2: t.x,
                      y2: tracksBottom,
                    }),
                  ),

                  // Shaded Range Selection Curtain (behind markers so markers pop!)
                  liveRange !== undefined &&
                    m('rect.pf-memtl__curtain', {
                      x: liveRange.x1,
                      y: CONTENT_TOP - 2,
                      width: Math.max(2, liveRange.x2 - liveRange.x1),
                      height: tracksBottom - CONTENT_TOP + 4,
                      rx: 4,
                    }),

                  // TRACK 0: Smaps snapshot markers (green circular pins)
                  smaps.map((s, i) => {
                    const x = xOfTs(s.ts);
                    const cy = trackCenterY(0);
                    const isEnd =
                      selection !== undefined &&
                      nearestByTs(smaps, selection.sel)?.ts === s.ts;
                    const isBase =
                      selection?.base !== undefined &&
                      nearestByTs(smaps, selection.base)?.ts === s.ts &&
                      !isEnd;
                    const isHovered = !isDragging && hoverSnap?.ts === s.ts;
                    const inWin = isInRange(s.ts);
                    const r = isEnd || isBase || isHovered ? 6.5 : 5;
                    return m(
                      'g.pf-memtl__marker.pf-memtl__marker--smaps',
                      {
                        key: `smap-${s.ts.toString()}`,
                        class: classNames(
                          isEnd && 'pf-memtl__marker--sel',
                          isBase && 'pf-memtl__marker--base',
                          isHovered && 'pf-memtl__marker--hovered',
                          inWin && 'pf-memtl__marker--in-range',
                        ),
                      },
                      [
                        (isEnd || isBase || isHovered) &&
                          m('circle.pf-memtl__marker-halo', {
                            cx: x,
                            cy,
                            r: r + 4,
                          }),
                        m('circle.pf-memtl__smaps-dot', {cx: x, cy, r}),
                        m('circle.pf-memtl__smaps-core', {
                          cx: x,
                          cy,
                          r: isEnd || isBase || isHovered ? 2.2 : 1.6,
                        }),
                        m(
                          'title',
                          `Smaps #${i + 1} · t=${secOf(s.ts)} · ${formatBytes(s.total)}`,
                        ),
                      ],
                    );
                  }),

                  // TRACK 1: Java Heap Dump markers (blue faceted diamonds + #idx)
                  procDumps.map((d, i) => {
                    const x = xOfTs(d.ts);
                    const cy = trackCenterY(1);
                    const isActive =
                      activeDump !== null &&
                      activeDump.upid === d.upid &&
                      activeDump.ts === d.ts;
                    const isHovered = !isDragging && hoverDump?.ts === d.ts;
                    const inWin = isInRange(d.ts);
                    const heapBytes = javaHeapBytesByTs.get(d.ts);
                    const r = isActive || isHovered ? 8 : 6.5;
                    const pts = `${x},${cy - r} ${x + r},${cy} ${x},${cy + r} ${x - r},${cy}`;
                    const innerR = r * 0.36;
                    const innerPts = `${x},${cy - innerR} ${x + innerR},${cy} ${x},${cy + innerR} ${x - innerR},${cy}`;
                    return m(
                      'g.pf-memtl__marker.pf-memtl__marker--java',
                      {
                        key: `dump-${i}-${d.ts.toString()}`,
                        class: classNames(
                          isActive && 'pf-memtl__marker--active',
                          isHovered && 'pf-memtl__marker--hovered',
                          inWin && 'pf-memtl__marker--in-range',
                        ),
                      },
                      [
                        (isActive || isHovered) &&
                          m('circle.pf-memtl__marker-halo', {
                            cx: x,
                            cy,
                            r: r + 4.5,
                          }),
                        m('polygon.pf-memtl__dump-diamond', {points: pts}),
                        m('polygon.pf-memtl__dump-core', {points: innerPts}),
                        m(
                          'text.pf-memtl__dump-tag',
                          {x: x + r + 5, y: cy + 3.5},
                          `#${i + 1}`,
                        ),
                        m(
                          'title',
                          `Java Heap Dump #${i + 1} · t=${secOf(d.ts)}` +
                            (heapBytes !== undefined
                              ? ` · ${formatBytes(heapBytes)} reachable`
                              : ''),
                        ),
                      ],
                    );
                  }),

                  // TRACK 2: Native heapprofd sample markers (orange capsules)
                  nativeSnaps.map((n, i) => {
                    const x = xOfTs(n.ts);
                    const cy = trackCenterY(2);
                    const isEnd =
                      selection !== undefined &&
                      selection.base === undefined &&
                      nearestByTs(nativeSnaps, selection.sel)?.ts === n.ts;
                    const isHovered = !isDragging && hoverNative?.ts === n.ts;
                    const inWin = isInRange(n.ts);
                    const w = isEnd || isHovered ? 6.5 : 5;
                    const h = isEnd || isHovered ? 18 : 14;
                    return m(
                      'g.pf-memtl__marker.pf-memtl__marker--native',
                      {
                        key: `nat-${n.ts.toString()}`,
                        class: classNames(
                          isEnd && 'pf-memtl__marker--sel',
                          isHovered && 'pf-memtl__marker--hovered',
                          inWin && 'pf-memtl__marker--in-range',
                        ),
                      },
                      [
                        (isEnd || isHovered) &&
                          m('rect.pf-memtl__marker-halo', {
                            x: x - w / 2 - 3,
                            y: cy - h / 2 - 3,
                            width: w + 6,
                            height: h + 6,
                            rx: 5,
                          }),
                        m('rect.pf-memtl__native-pill', {
                          x: x - w / 2,
                          y: cy - h / 2,
                          width: w,
                          height: h,
                          rx: w / 2,
                        }),
                        m(
                          'title',
                          `Native sample #${i + 1} · t=${secOf(n.ts)} · ${formatDelta(n.deltaBytes)}`,
                        ),
                      ],
                    );
                  }),

                  // Single-point pinned selection line
                  !isDragRange &&
                    selection !== undefined &&
                    selection.base === undefined &&
                    m('g.pf-memtl__pin-line', [
                      m('line', {
                        x1: xOfTs(selection.sel),
                        y1: CONTENT_TOP - 4,
                        x2: xOfTs(selection.sel),
                        y2: tracksBottom + 4,
                      }),
                      m(
                        'text.pf-memtl__pin-label',
                        {
                          x: Math.min(
                            PAD_LEFT + plotW - 40,
                            Math.max(PAD_LEFT + 40, xOfTs(selection.sel)),
                          ),
                          y: CONTENT_TOP - 9,
                        },
                        `t=${secOf(selection.sel)}`,
                      ),
                    ]),

                  // Range Selection Edges + Draggable Handles + Floating Label
                  liveRange !== undefined &&
                    m('g.pf-memtl__selection', [
                      m('line.pf-memtl__edge.pf-memtl__edge--base', {
                        x1: liveRange.x1,
                        y1: CONTENT_TOP - 4,
                        x2: liveRange.x1,
                        y2: tracksBottom + 4,
                      }),
                      !isDragging &&
                        m('rect.pf-memtl__handle.pf-memtl__handle--base', {
                          x: liveRange.x1 - 5,
                          y: (CONTENT_TOP + tracksBottom) / 2 - 14,
                          width: 10,
                          height: 28,
                          rx: 3,
                          onpointerdown: (e: PointerEvent) =>
                            startDrag('resize-base', liveRange.x2, e),
                        }),
                      m('line.pf-memtl__edge.pf-memtl__edge--sel', {
                        x1: liveRange.x2,
                        y1: CONTENT_TOP - 4,
                        x2: liveRange.x2,
                        y2: tracksBottom + 4,
                      }),
                      !isDragging &&
                        m('rect.pf-memtl__handle.pf-memtl__handle--sel', {
                          x: liveRange.x2 - 5,
                          y: (CONTENT_TOP + tracksBottom) / 2 - 14,
                          width: 10,
                          height: 28,
                          rx: 3,
                          onpointerdown: (e: PointerEvent) =>
                            startDrag('resize-sel', liveRange.x1, e),
                        }),
                      m(
                        'text.pf-memtl__range-label',
                        {
                          x: Math.min(
                            PAD_LEFT + plotW - 84,
                            Math.max(
                              PAD_LEFT + 84,
                              (liveRange.x1 + liveRange.x2) / 2,
                            ),
                          ),
                          y: CONTENT_TOP - 9,
                        },
                        liveBaseTs !== undefined && liveSelTs !== undefined
                          ? `${secOf(liveBaseTs)} → ${secOf(liveSelTs)}` +
                              (liveHeaderDelta !== undefined
                                ? ` (${formatDelta(liveHeaderDelta)})`
                                : '')
                          : '',
                      ),
                    ]),

                  // Hover crosshair spanning RSS band + all 3 tracks
                  hoverX !== undefined &&
                    !isDragging &&
                    m('g', [
                      m('line.pf-memtl__crosshair', {
                        x1: hoverX,
                        y1: CONTENT_TOP,
                        x2: hoverX,
                        y2: tracksBottom,
                      }),
                      hasRss &&
                        hoverRss !== undefined &&
                        m('circle.pf-memtl__rss-dot', {
                          cx: hoverX,
                          cy: yOfRss(hoverRss.totalRssSwap),
                          r: 4,
                        }),
                      hasRss &&
                        hasAnonSplit &&
                        hoverRss?.anonSwap !== undefined &&
                        m('circle.pf-memtl__rss-dot--anon', {
                          cx: hoverX,
                          cy: yOfRss(hoverRss.anonSwap),
                          r: 3,
                        }),
                    ]),

                  // Time axis baseline + tick labels
                  m('line.pf-memtl__xaxis', {
                    x1: PAD_LEFT,
                    y1: tracksBottom + 6,
                    x2: PAD_LEFT + plotW,
                    y2: tracksBottom + 6,
                  }),
                  xTicks.map((t, i) =>
                    m(
                      'text.pf-memtl__xlabel',
                      {key: `x-${i}`, x: t.x, y: axisY + 4},
                      t.label,
                    ),
                  ),
                ],
              ),

              // Floating hover HUD pill
              hoverTs !== undefined &&
                !isDragging &&
                m('.pf-memtl__hud', [
                  m('span.pf-memtl__hud-ts', `t=${secOf(hoverTs)}`),
                  hoverRss !== undefined &&
                    m(
                      'span.pf-memtl__hud-rss',
                      `RSS+Swap: ${formatBytes(hoverRss.totalRssSwap)}` +
                        (hasAnonSplit && hoverRss.anonSwap !== undefined
                          ? ` (Anon+Swap ${formatBytes(hoverRss.anonSwap)})`
                          : ''),
                    ),
                  hoverSnap !== undefined &&
                    m(
                      'span.pf-memtl__hud-smaps',
                      `● Smaps: ${formatBytes(hoverSnap.total)}`,
                    ),
                  hoverDump !== undefined &&
                    m(
                      'span.pf-memtl__hud-dump',
                      `◆ Java Dump #${procDumps.indexOf(hoverDump) + 1}` +
                        (hoverDumpBytes !== undefined
                          ? ` (${formatBytes(hoverDumpBytes)})`
                          : ''),
                    ),
                  hoverNative !== undefined &&
                    m(
                      'span.pf-memtl__hud-native',
                      `▮ Native: ${formatDelta(hoverNative.deltaBytes)}`,
                    ),
                ]),
            ]),

        // Inline Memory Delta Breakdown Bar
        this.renderDeltaBreakdown(trace, smaps, selection),
      ]),
    );
  }

  private renderHeaderStatus(opts: {
    readonly isComparing: boolean;
    readonly selection?: MemSelection;
    readonly liveDelta?: number;
    readonly liveSelSnap?: SmapsSnap;
    readonly liveSelRss?: RssPoint;
    readonly secOf: (ts: time) => string;
    readonly canCompareFullTrace: boolean;
    readonly onCompareFullTrace: () => void;
    readonly onClear: () => void;
  }): m.Children {
    const {
      isComparing,
      selection,
      liveDelta,
      liveSelSnap,
      liveSelRss,
      secOf,
      canCompareFullTrace,
      onCompareFullTrace,
      onClear,
    } = opts;

    const latestTotal = liveSelSnap?.total ?? liveSelRss?.totalRssSwap;
    const latestTs = liveSelSnap?.ts ?? liveSelRss?.ts;

    return m('.pf-memtl__status-controls', [
      isComparing && selection?.base !== undefined
        ? m('.pf-memtl__delta-badge', [
            m(Icon, {icon: 'compare_arrows'}),
            m('span', `Δ ${secOf(selection.base)} → ${secOf(selection.sel)}`),
            liveDelta !== undefined &&
              m(
                'span.pf-memtl__delta-badge-val',
                {style: {color: deltaColor(liveDelta)}},
                formatDelta(liveDelta),
              ),
          ])
        : selection !== undefined
          ? m('.pf-memtl__pin-badge', [
              m(Icon, {icon: 'photo_camera'}),
              m(
                'span',
                `Pinned @ ${secOf(selection.sel)}` +
                  (latestTotal !== undefined
                    ? ` · ${formatBytes(latestTotal)}`
                    : ''),
              ),
            ])
          : latestTs !== undefined &&
            latestTotal !== undefined &&
            m('.pf-memtl__pin-badge', [
              m(Icon, {icon: 'photo_camera'}),
              m(
                'span',
                `Latest @ ${secOf(latestTs)} · ${formatBytes(latestTotal)}`,
              ),
            ]),
      !isComparing &&
        canCompareFullTrace &&
        m(Button, {
          label: 'Compare full trace Δ',
          icon: 'compare_arrows',
          variant: ButtonVariant.Outlined,
          compact: true,
          onclick: onCompareFullTrace,
        }),
      selection !== undefined &&
        m(Button, {
          label: isComparing ? 'Clear range' : 'Reset',
          icon: 'close',
          variant: ButtonVariant.Minimal,
          compact: true,
          onclick: onClear,
        }),
    ]);
  }

  private renderDeltaBreakdown(
    trace: Trace,
    smaps: ReadonlyArray<SmapsSnap>,
    selection?: MemSelection,
  ): m.Children {
    if (smaps.length < 2) return null;
    const firstSnap = smaps[0];
    const lastSnap = smaps[smaps.length - 1];
    if (firstSnap === undefined || lastSnap === undefined) return null;

    const sel =
      (selection !== undefined
        ? nearestByTs(smaps, selection.sel)
        : undefined) ?? lastSnap;
    const resolvedBase =
      selection?.base !== undefined
        ? nearestByTs(smaps, selection.base)
        : undefined;
    const diffing = resolvedBase !== undefined && resolvedBase.ts !== sel.ts;
    const base = diffing ? resolvedBase : firstSnap;
    const wholeTrace = base.ts === firstSnap.ts && sel.ts === lastSnap.ts;

    const total = sel.total - base.total;
    const shrinking = total < 0;
    const spanS = Number(sel.ts - base.ts) / 1e9;
    const ratePerHour = spanS > 0 ? (total / spanS) * 3600 : undefined;

    const secOf = (ts: time) =>
      `${(Number(ts - trace.traceInfo.start) / 1e9).toFixed(1)}s`;

    const categories = SMAPS_CATEGORIES.map((c) => ({
      name: c.label,
      color: c.color,
      delta:
        (sel.byCategory.get(c.key) ?? 0) - (base.byCategory.get(c.key) ?? 0),
    }));

    return m(
      Inset,
      {
        className: classNames(
          'pf-memscope-growth',
          diffing && 'pf-memtl__growth--active',
        ),
      },
      [
        m('.pf-memscope-growth__label', [
          diffing
            ? `Selected Range Delta (${secOf(base.ts)} → ${secOf(sel.ts)})`
            : 'Memory delta',
        ]),
        m('.pf-memscope-growth__headline', [
          m(
            'span.pf-memscope-growth__delta',
            {style: {color: deltaColor(total)}},
            formatDelta(total),
          ),
          m(
            'span.pf-memscope-growth__context',
            `${shrinking ? 'removed' : 'added'} ` +
              (diffing
                ? 'in selected window'
                : wholeTrace
                  ? 'over the trace'
                  : 'up to pinned snapshot'),
          ),
          ratePerHour !== undefined &&
            m(
              'span.pf-memscope-growth__context',
              `· ${formatDelta(ratePerHour)}/hour`,
            ),
        ]),
        m(ProportionBar, {
          segments: categories.map((c) => ({
            label: c.name,
            weight: (shrinking ? c.delta < 0 : c.delta > 0)
              ? Math.abs(c.delta)
              : 0,
            color: c.color,
            value: formatDelta(c.delta),
            valueColor: deltaColor(c.delta),
          })),
        }),
      ],
    );
  }
}
