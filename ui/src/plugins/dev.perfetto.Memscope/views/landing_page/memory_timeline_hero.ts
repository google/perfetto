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

// Hero Interactive Memory Timeline: optional full-width process RSS + Swap
// counter graph with a short combined sample-sprite strip (Smaps, Java Heap,
// Native) directly underneath on one shared time axis. Drag anywhere across
// the timeline to select a [base → sel] range and switch every section below
// into Δ mode; click any sprite to pin a snapshot or dump.

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
  readonly anonSwapBytes: number;
}

interface HeroTimelineData {
  readonly smaps: ReadonlyArray<SmapsSnap>;
  readonly nativeSnaps: ReadonlyArray<NativeSnap>;
  readonly javaHeapBytesByTs: ReadonlyMap<bigint, number>;
  readonly rssPoints: ReadonlyArray<RssPoint>;
  readonly rssPlotPoints: ReadonlyArray<RssPoint>;
  readonly rssMaxBytes: number;
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
      if (minPt === undefined || pt.anonSwapBytes < minPt.anonSwapBytes) {
        minPt = pt;
      }
      if (maxPt === undefined || pt.anonSwapBytes > maxPt.anonSwapBytes) {
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
}> {
  const rssPoints: RssPoint[] = [];
  let rssMaxBytes = 0;

  try {
    const res = await trace.engine.query(`
      SELECT
        c.ts AS ts,
        t.name AS name,
        CAST(c.value AS INT) AS val
      FROM counter c
      JOIN process_counter_track t ON c.track_id = t.id
      WHERE t.upid = ${upid}
        AND t.name IN ('mem.rss.anon', 'mem.swap')
      ORDER BY c.ts ASC
    `);

    let anon = 0;
    let swap = 0;
    let curTs: bigint | undefined;

    const emit = (tsRaw: bigint) => {
      const anonSwapBytes = Math.max(0, anon + swap);
      if (anonSwapBytes > rssMaxBytes) rssMaxBytes = anonSwapBytes;
      const prev = rssPoints[rssPoints.length - 1];
      if (prev !== undefined && prev.anonSwapBytes === anonSwapBytes) {
        return;
      }
      rssPoints.push({
        ts: Time.fromRaw(tsRaw),
        anonSwapBytes,
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
      if (it.name === 'mem.rss.anon') {
        anon = v;
      } else if (it.name === 'mem.swap') {
        swap = v;
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
    };
  }

  return {
    rssPoints,
    rssPlotPoints: downsampleRssPoints(rssPoints),
    rssMaxBytes,
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
    // Module unavailable or empty — pins still render on the sample strip.
  }

  // 4. Optional process RSS + Swap counter series (drawn above sample strip)
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

const PAD_X = 14;
const CONTENT_TOP = 22;
const RSS_BAND_H = 82;
const RSS_INNER_PAD_Y = 7;
const STRIP_GAP = 6;
const STRIP_H = 28;
const DRAG_THRESHOLD_PX = 6;

type DragMode = 'range' | 'resize-base' | 'resize-sel';

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
  for (let i = 1; i < points.length; i++) {
    const pt = points[i];
    if (pt === undefined) continue;
    lineParts.push(`H ${pt.x.toFixed(1)} V ${pt.y.toFixed(1)}`);
    prevX = pt.x;
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
    const hasRss = rssPlotPoints.length > 0 && rssMaxBytes > 0;
    const totalSamples = smaps.length + procDumps.length + nativeSnaps.length;

    // Compact full-width layout: optional RSS + Swap counter graph on top,
    // short combined sample-sprite track directly beneath.
    const rssTop = CONTENT_TOP;
    const rssBottom = rssTop + RSS_BAND_H;
    const stripTop = hasRss ? rssBottom + STRIP_GAP : CONTENT_TOP;
    const stripBottom = stripTop + STRIP_H;
    const stripMidY = stripTop + STRIP_H / 2;
    const axisY = stripBottom + 15;
    const svgHeight = axisY + 9;

    const traceStart = trace.traceInfo.start;
    const traceEnd = trace.traceInfo.end;
    const spanNs = Math.max(1, Number(traceEnd - traceStart));
    const durationS = spanNs / 1e9;

    const svgW = this.containerWidth;
    const plotW = Math.max(120, svgW - PAD_X * 2);

    const xOfTs = (ts: time): number => {
      const frac = Math.min(1, Math.max(0, Number(ts - traceStart) / spanNs));
      return PAD_X + frac * plotW;
    };

    const tsOfX = (x: number): time => {
      const clamped = Math.min(PAD_X + plotW, Math.max(PAD_X, x));
      const frac = (clamped - PAD_X) / plotW;
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
        ? liveSelRss.anonSwapBytes - liveBaseRss.anonSwapBytes
        : undefined;
    const liveHeaderDelta = liveSmapsDelta ?? liveRssDelta;

    // Precompute SVG path for RSS Anon + Swap counter curve.
    const rssPaths = hasRss
      ? buildStepPaths(
          rssPlotPoints.map((p) => ({
            x: xOfTs(p.ts),
            y: yOfRss(p.anonSwapBytes),
          })),
          rssPlotBottom,
          PAD_X + plotW,
        )
      : undefined;

    // X-axis ticks (5–9 clean intervals).
    const xTickCount = Math.max(4, Math.min(9, Math.floor(plotW / 115)));
    const xTicks: {x: number; label: string}[] = [];
    for (let i = 0; i <= xTickCount; i++) {
      const frac = i / xTickCount;
      xTicks.push({
        x: PAD_X + frac * plotW,
        label: `${(frac * durationS).toFixed(durationS < 10 ? 1 : 0)}s`,
      });
    }

    const localX = (e: PointerEvent): number => {
      const svg = e.currentTarget as SVGSVGElement;
      const rect = svg.getBoundingClientRect();
      return Math.min(PAD_X + plotW, Math.max(PAD_X, e.clientX - rect.left));
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
          PAD_X + plotW,
          Math.max(PAD_X, e.clientX - rect.left),
        );
        this.dragMode = mode;
        this.dragAnchorX = anchorX;
        this.dragCurrentX = curX;
        this.hoverX = undefined;
      }
    };

    const onPointerDown = (e: PointerEvent) => {
      startDrag('range', localX(e), e);
    };

    const onPointerMove = (e: PointerEvent) => {
      const x = localX(e);
      if (this.dragMode !== undefined) {
        this.dragCurrentX = x;
      } else {
        this.hoverX = x;
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

      // Click: snap to nearest sprite across Java / Smaps / Native (preferring
      // an exact Java dump hit when sprites overlap).
      const clickX = this.dragCurrentX;
      const clickTs = tsOfX(clickX);
      const nearDump =
        procDumps.length > 0 ? nearestByTs(procDumps, clickTs) : undefined;
      if (
        nearDump !== undefined &&
        Math.abs(xOfTs(nearDump.ts) - clickX) <= 10
      ) {
        onSelect({sel: nearDump.ts});
        onSelectDump?.(nearDump);
        return;
      }

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
      }
    };

    // Hover HUD readout
    const hoverX = this.hoverX;
    const hoverTs = hoverX !== undefined ? tsOfX(hoverX) : undefined;
    const hoverRss =
      hoverTs !== undefined ? rssPointAtTs(rssPoints, hoverTs) : undefined;
    const pickNear = <T extends {readonly ts: time}>(
      items: ReadonlyArray<T>,
      radiusPx: number,
    ): T | undefined => {
      if (hoverX === undefined || hoverTs === undefined || items.length === 0) {
        return undefined;
      }
      const nearest = nearestByTs(items, hoverTs);
      if (nearest === undefined) return undefined;
      return Math.abs(xOfTs(nearest.ts) - hoverX) <= radiusPx
        ? nearest
        : undefined;
    };
    const hoverSnap = pickNear(smaps, 12);
    const hoverDump = pickNear(procDumps, 14);
    const hoverNative = pickNear(nativeSnaps, 10);
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

    const isInRange = (ts: time): boolean => {
      if (liveBaseTs === undefined || liveSelTs === undefined) return false;
      const lo = liveBaseTs < liveSelTs ? liveBaseTs : liveSelTs;
      const hi = liveBaseTs < liveSelTs ? liveSelTs : liveBaseTs;
      return ts >= lo && ts <= hi;
    };

    const gradId = `pf-memtl-rss-grad-${upid}`;

    return m(
      Panel,
      {className: 'pf-memtl-hero'},
      m(Panel.Header, {
        title: 'Memory Timeline',
        subtitle:
          'Drag across the timeline to select a time range and view all ' +
          'sections below as a delta (Δ). Click any marker to pin.',
        controls: headerStatus,
      }),
      m(Panel.Body, {className: 'pf-memtl-hero__body'}, [
        data === undefined
          ? m('.pf-memscope-placeholder', 'Loading memory timeline…')
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
                  // Gradient definition for optional RSS Anon + Swap counter graph
                  hasRss &&
                    m(
                      'defs',
                      m(
                        'linearGradient',
                        {id: gradId, x1: '0', y1: '0', x2: '0', y2: '1'},
                        [
                          m('stop', {
                            'offset': '0%',
                            'stop-color': '#7c4dff',
                            'stop-opacity': '0.24',
                          }),
                          m('stop', {
                            'offset': '100%',
                            'stop-color': '#7c4dff',
                            'stop-opacity': '0.02',
                          }),
                        ],
                      ),
                    ),

                  // Optional full-width RSS Anon + Swap counter graph
                  hasRss &&
                    rssPaths !== undefined &&
                    m('g.pf-memtl__rss-band', [
                      m('rect.pf-memtl__track-bg', {
                        x: PAD_X,
                        y: rssTop,
                        width: plotW,
                        height: RSS_BAND_H,
                        rx: 6,
                      }),
                      m('line.pf-memtl__rss-hgrid', {
                        x1: PAD_X + 1,
                        y1: (rssPlotTop + rssPlotBottom) / 2,
                        x2: PAD_X + plotW - 1,
                        y2: (rssPlotTop + rssPlotBottom) / 2,
                      }),
                      m('path.pf-memtl__rss-area', {
                        d: rssPaths.area,
                        fill: `url(#${gradId})`,
                      }),
                      m('path.pf-memtl__rss-line', {
                        d: rssPaths.line,
                      }),
                      m(
                        'text.pf-memtl__rss-ytick',
                        {x: PAD_X + 7, y: rssTop + 13},
                        formatBytes(rssMaxBytes),
                      ),
                      m(
                        'text.pf-memtl__rss-ytick',
                        {x: PAD_X + 7, y: rssBottom - 4},
                        '0',
                      ),
                    ]),

                  // Short combined sample-sprite strip beneath the graph
                  m(
                    'g.pf-memtl__strip',
                    {
                      class: classNames(
                        totalSamples === 0 && 'pf-memtl__track--empty',
                      ),
                    },
                    [
                      m('rect.pf-memtl__track-bg', {
                        x: PAD_X,
                        y: stripTop,
                        width: plotW,
                        height: STRIP_H,
                        rx: 6,
                      }),
                      m('line.pf-memtl__track-rail', {
                        x1: PAD_X + 8,
                        y1: stripMidY,
                        x2: PAD_X + plotW - 8,
                        y2: stripMidY,
                      }),
                      totalSamples === 0 &&
                        m(
                          'text.pf-memtl__track-empty-msg',
                          {x: PAD_X + plotW / 2, y: stripMidY + 4},
                          'No memory snapshots or heap profiles captured',
                        ),
                    ],
                  ),

                  // Subtle vertical time gridlines spanning graph + strip
                  xTicks.map((t, i) =>
                    m('line.pf-memtl__vgrid', {
                      key: `vg-${i}`,
                      x1: t.x,
                      y1: CONTENT_TOP,
                      x2: t.x,
                      y2: stripBottom,
                    }),
                  ),

                  // Shaded Range Selection Curtain (behind sprites so sprites pop!)
                  liveRange !== undefined &&
                    m('rect.pf-memtl__curtain', {
                      x: liveRange.x1,
                      y: CONTENT_TOP - 2,
                      width: Math.max(2, liveRange.x2 - liveRange.x1),
                      height: stripBottom - CONTENT_TOP + 4,
                      rx: 4,
                    }),

                  // Native heapprofd capsules (drawn first so dots/diamonds sit crisply above)
                  nativeSnaps.map((n, i) => {
                    const x = xOfTs(n.ts);
                    const isEnd =
                      selection !== undefined &&
                      selection.base === undefined &&
                      nearestByTs(nativeSnaps, selection.sel)?.ts === n.ts;
                    const isHovered = !isDragging && hoverNative?.ts === n.ts;
                    const inWin = isInRange(n.ts);
                    const w = isEnd || isHovered ? 6 : 4.5;
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
                            y: stripMidY - h / 2 - 3,
                            width: w + 6,
                            height: h + 6,
                            rx: 5,
                          }),
                        m('rect.pf-memtl__native-pill', {
                          x: x - w / 2,
                          y: stripMidY - h / 2,
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

                  // Smaps circular pins
                  smaps.map((s, i) => {
                    const x = xOfTs(s.ts);
                    const isEnd =
                      selection !== undefined &&
                      nearestByTs(smaps, selection.sel)?.ts === s.ts;
                    const isBase =
                      selection?.base !== undefined &&
                      nearestByTs(smaps, selection.base)?.ts === s.ts &&
                      !isEnd;
                    const isHovered = !isDragging && hoverSnap?.ts === s.ts;
                    const inWin = isInRange(s.ts);
                    const r = isEnd || isBase || isHovered ? 6 : 4.5;
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
                            cy: stripMidY,
                            r: r + 3.5,
                          }),
                        m('circle.pf-memtl__smaps-dot', {
                          cx: x,
                          cy: stripMidY,
                          r,
                        }),
                        m('circle.pf-memtl__smaps-core', {
                          cx: x,
                          cy: stripMidY,
                          r: isEnd || isBase || isHovered ? 2 : 1.5,
                        }),
                        m(
                          'title',
                          `Smaps #${i + 1} · t=${secOf(s.ts)} · ${formatBytes(s.total)}`,
                        ),
                      ],
                    );
                  }),

                  // Java Heap Dump faceted diamonds (on top so rare dumps always stand out)
                  procDumps.map((d, i) => {
                    const x = xOfTs(d.ts);
                    const isActive =
                      activeDump !== null &&
                      activeDump.upid === d.upid &&
                      activeDump.ts === d.ts;
                    const isHovered = !isDragging && hoverDump?.ts === d.ts;
                    const inWin = isInRange(d.ts);
                    const heapBytes = javaHeapBytesByTs.get(d.ts);
                    const r = isActive || isHovered ? 7.5 : 6;
                    const pts = `${x},${stripMidY - r} ${x + r},${stripMidY} ${x},${stripMidY + r} ${x - r},${stripMidY}`;
                    const innerR = r * 0.36;
                    const innerPts = `${x},${stripMidY - innerR} ${x + innerR},${stripMidY} ${x},${stripMidY + innerR} ${x - innerR},${stripMidY}`;
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
                            cy: stripMidY,
                            r: r + 4,
                          }),
                        m('polygon.pf-memtl__dump-diamond', {points: pts}),
                        m('polygon.pf-memtl__dump-core', {points: innerPts}),
                        m(
                          'text.pf-memtl__dump-tag',
                          {x: x + r + 4, y: stripMidY + 3.5},
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

                  // Single-point pinned selection line
                  !isDragRange &&
                    selection !== undefined &&
                    selection.base === undefined &&
                    m('g.pf-memtl__pin-line', [
                      m('line', {
                        x1: xOfTs(selection.sel),
                        y1: CONTENT_TOP - 4,
                        x2: xOfTs(selection.sel),
                        y2: stripBottom + 4,
                      }),
                      m(
                        'text.pf-memtl__pin-label',
                        {
                          x: Math.min(
                            PAD_X + plotW - 40,
                            Math.max(PAD_X + 40, xOfTs(selection.sel)),
                          ),
                          y: CONTENT_TOP - 8,
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
                        y2: stripBottom + 4,
                      }),
                      !isDragging &&
                        m('rect.pf-memtl__handle.pf-memtl__handle--base', {
                          x: liveRange.x1 - 5,
                          y: (CONTENT_TOP + stripBottom) / 2 - 14,
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
                        y2: stripBottom + 4,
                      }),
                      !isDragging &&
                        m('rect.pf-memtl__handle.pf-memtl__handle--sel', {
                          x: liveRange.x2 - 5,
                          y: (CONTENT_TOP + stripBottom) / 2 - 14,
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
                            PAD_X + plotW - 84,
                            Math.max(
                              PAD_X + 84,
                              (liveRange.x1 + liveRange.x2) / 2,
                            ),
                          ),
                          y: CONTENT_TOP - 8,
                        },
                        liveBaseTs !== undefined && liveSelTs !== undefined
                          ? `${secOf(liveBaseTs)} → ${secOf(liveSelTs)}` +
                              (liveHeaderDelta !== undefined
                                ? ` (${formatDelta(liveHeaderDelta)})`
                                : '')
                          : '',
                      ),
                    ]),

                  // Hover crosshair spanning RSS graph + combined sprite strip
                  hoverX !== undefined &&
                    !isDragging &&
                    m('g', [
                      m('line.pf-memtl__crosshair', {
                        x1: hoverX,
                        y1: CONTENT_TOP,
                        x2: hoverX,
                        y2: stripBottom,
                      }),
                      hasRss &&
                        hoverRss !== undefined &&
                        m('circle.pf-memtl__rss-dot', {
                          cx: hoverX,
                          cy: yOfRss(hoverRss.anonSwapBytes),
                          r: 4,
                        }),
                    ]),

                  // Time axis baseline + tick labels
                  m('line.pf-memtl__xaxis', {
                    x1: PAD_X,
                    y1: stripBottom + 5,
                    x2: PAD_X + plotW,
                    y2: stripBottom + 5,
                  }),
                  xTicks.map((t, i) =>
                    m(
                      'text.pf-memtl__xlabel',
                      {key: `x-${i}`, x: t.x, y: axisY + 3},
                      t.label,
                    ),
                  ),
                ],
              ),

              // Compact inline legend bar underneath (replaces left track names)
              m('.pf-memtl__legend', [
                hasRss &&
                  m('span.pf-memtl__legend-item', [
                    m('span.pf-memtl__legend-line.pf-memtl__legend-line--rss'),
                    'RSS Anon + Swap',
                  ]),
                m(
                  'span.pf-memtl__legend-item',
                  {
                    className: classNames(
                      smaps.length === 0 && 'pf-memtl__legend-item--empty',
                    ),
                  },
                  [
                    m(
                      'span.pf-memtl__legend-sprite.pf-memtl__legend-sprite--smaps',
                    ),
                    `Smaps (${smaps.length})`,
                  ],
                ),
                m(
                  'span.pf-memtl__legend-item',
                  {
                    className: classNames(
                      procDumps.length === 0 && 'pf-memtl__legend-item--empty',
                    ),
                  },
                  [
                    m(
                      'span.pf-memtl__legend-sprite.pf-memtl__legend-sprite--java',
                    ),
                    `Java Heap (${procDumps.length})`,
                  ],
                ),
                m(
                  'span.pf-memtl__legend-item',
                  {
                    className: classNames(
                      nativeSnaps.length === 0 &&
                        'pf-memtl__legend-item--empty',
                    ),
                  },
                  [
                    m(
                      'span.pf-memtl__legend-sprite.pf-memtl__legend-sprite--native',
                    ),
                    `Native (${nativeSnaps.length})`,
                  ],
                ),
              ]),

              // Floating hover HUD pill
              hoverTs !== undefined &&
                !isDragging &&
                m('.pf-memtl__hud', [
                  m('span.pf-memtl__hud-ts', `t=${secOf(hoverTs)}`),
                  hoverRss !== undefined &&
                    m(
                      'span.pf-memtl__hud-rss',
                      `RSS Anon + Swap: ${formatBytes(hoverRss.anonSwapBytes)}`,
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

    const latestTotal = liveSelSnap?.total ?? liveSelRss?.anonSwapBytes;
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
