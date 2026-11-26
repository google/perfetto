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

// Hero Interactive Memory Timeline: sits front-and-center at the top of the
// unified Memory page, plotting memory over time together with explicit sample
// markers (smaps snapshots, Java heap dumps, heapprofd native profiles).
// Dragging anywhere across the timeline selects a [base → sel] time window and
// switches the entire page below into Δ comparison mode.

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
import {niceKbInterval} from '../../utils';
import {deltaColor, formatBytes, formatDelta} from './mem_format';
import {type MemSelection, nearestByTs} from './selection';
import {SMAPS_CATEGORIES, SMAPS_CATEGORY_CASE_SQL} from './smaps_categories';
import './memory_timeline_hero.scss';

export interface SmapsSnap {
  readonly ts: time;
  readonly total: number;
  readonly byCategory: ReadonlyMap<string, number>;
}

interface CounterPoint {
  readonly ts: time;
  readonly bytes: number;
}

interface HeroTimelineData {
  readonly smaps: ReadonlyArray<SmapsSnap>;
  readonly rssCounters: ReadonlyArray<CounterPoint>;
  readonly nativeTs: ReadonlyArray<time>;
  readonly javaHeapBytesByTs: ReadonlyMap<bigint, number>;
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

  // 2. Fallback RSS counter curve when smaps has < 2 snapshots
  const rssCounters: CounterPoint[] = [];
  if (smaps.length < 2) {
    const rssRes = await trace.engine.query(`
      SELECT c.ts AS ts, CAST(c.value AS INT) AS bytes
      FROM counter c
      JOIN process_counter_track t ON c.track_id = t.id
      WHERE t.upid = ${upid} AND t.name = 'mem.rss'
      ORDER BY c.ts ASC
    `);
    const raw: CounterPoint[] = [];
    for (
      const it = rssRes.iter({ts: LONG, bytes: NUM});
      it.valid();
      it.next()
    ) {
      raw.push({ts: Time.fromRaw(it.ts), bytes: Math.max(0, it.bytes)});
    }
    // Downsample if there are thousands of counter ticks.
    const maxPts = 360;
    const step = raw.length > maxPts ? Math.ceil(raw.length / maxPts) : 1;
    for (let i = 0; i < raw.length; i += step) {
      const pt = raw[i];
      if (pt !== undefined) rssCounters.push(pt);
    }
    const lastRaw = raw[raw.length - 1];
    if (
      lastRaw !== undefined &&
      rssCounters[rssCounters.length - 1] !== lastRaw
    ) {
      rssCounters.push(lastRaw);
    }
  }

  // 3. Native heapprofd sample timestamps
  const nativeTs: time[] = [];
  const nativeRes = await trace.engine.query(`
    SELECT DISTINCT a.ts AS ts
    FROM heap_profile_allocation a
    WHERE a.upid = ${upid}
    ORDER BY a.ts ASC
  `);
  for (const it = nativeRes.iter({ts: LONG}); it.valid(); it.next()) {
    nativeTs.push(Time.fromRaw(it.ts));
  }

  // 4. Optional Java heap size per dump (used when no smaps/RSS curve exists)
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
    // Module unavailable or empty — pins still plot on the time axis.
  }

  return {smaps, rssCounters, nativeTs, javaHeapBytesByTs};
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

const PAD_LEFT = 62;
const PAD_RIGHT = 20;
const CHART_TOP = 28;
const CHART_BOTTOM = 152;
const LANE_Y = 172;
const AXIS_Y = 194;
const SVG_HEIGHT = 212;
const DRAG_THRESHOLD_PX = 6;

type DragMode = 'range' | 'resize-base' | 'resize-sel';

export class MemoryTimelineHero implements m.ClassComponent<MemoryTimelineHeroAttrs> {
  private readonly slot = new AsyncMemo<HeroTimelineData>();
  private resizeObserver?: ResizeObserver;
  private containerWidth = 960;

  // Interactive pointer drag & hover state (in SVG pixel space)
  private dragMode?: DragMode;
  private dragAnchorX = 0;
  private dragCurrentX = 0;
  private hoverX?: number;

  oncreate({dom}: m.VnodeDOM<MemoryTimelineHeroAttrs>) {
    const wrap = dom.querySelector('.pf-memtl__canvas-wrap');
    if (wrap instanceof HTMLElement) {
      const update = () => {
        const w = Math.max(320, Math.floor(wrap.clientWidth));
        if (w !== this.containerWidth) {
          this.containerWidth = w;
          m.redraw();
        }
      };
      update();
      this.resizeObserver = new ResizeObserver(update);
      this.resizeObserver.observe(wrap);
    }
  }

  onremove() {
    this.resizeObserver?.disconnect();
    this.resizeObserver = undefined;
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
    const rssCounters = data?.rssCounters ?? [];
    const nativeTs = data?.nativeTs ?? [];
    const javaHeapBytesByTs =
      data?.javaHeapBytesByTs ?? new Map<bigint, number>();

    const traceStart = trace.traceInfo.start;
    const traceEnd = trace.traceInfo.end;
    const spanNs = Math.max(1, Number(traceEnd - traceStart));
    const durationS = spanNs / 1e9;

    const svgW = this.containerWidth;
    const plotW = Math.max(100, svgW - PAD_LEFT - PAD_RIGHT);
    const plotH = CHART_BOTTOM - CHART_TOP;

    const xOfTs = (ts: time): number => {
      const frac = Math.min(1, Math.max(0, Number(ts - traceStart) / spanNs));
      return PAD_LEFT + frac * plotW;
    };

    const tsOfX = (x: number): time => {
      const clamped = Math.min(PAD_LEFT + plotW, Math.max(PAD_LEFT, x));
      const frac = (clamped - PAD_LEFT) / plotW;
      return Time.fromRaw(traceStart + BigInt(Math.round(frac * spanNs)));
    };

    const secOf = (ts: time): string =>
      `${(Number(ts - traceStart) / 1e9).toFixed(1)}s`;

    // Determine Y scale across whichever series we are plotting.
    let rawMaxBytes = 0;
    for (const s of smaps) rawMaxBytes = Math.max(rawMaxBytes, s.total);
    if (smaps.length < 2) {
      for (const c of rssCounters) rawMaxBytes = Math.max(rawMaxBytes, c.bytes);
    }
    if (rawMaxBytes === 0) {
      for (const bytes of javaHeapBytesByTs.values()) {
        rawMaxBytes = Math.max(rawMaxBytes, bytes);
      }
    }
    const yStep =
      rawMaxBytes > 0 ? niceKbInterval(rawMaxBytes / 1024) * 1024 : 1024 * 1024;
    const yMax = Math.max(yStep, Math.ceil(rawMaxBytes / yStep) * yStep);
    const yTicks: number[] = [];
    for (let v = 0; v <= yMax; v += yStep) {
      yTicks.push(v);
      if (yTicks.length >= 6) break;
    }
    const yOfBytes = (bytes: number): number =>
      CHART_BOTTOM - Math.min(1, Math.max(0, bytes / yMax)) * plotH;

    // Build stacked area polygons for smaps categories (bottom to top).
    const activeCategories = SMAPS_CATEGORIES.filter((cat) =>
      smaps.some((s) => (s.byCategory.get(cat.key) ?? 0) > 0),
    );
    const stackedAreas: {key: string; color: string; path: string}[] = [];
    if (smaps.length >= 2) {
      const running = new Array<number>(smaps.length).fill(0);
      for (const cat of activeCategories) {
        const lowerY = running.map((v) => yOfBytes(v));
        for (let i = 0; i < smaps.length; i++) {
          const snap = smaps[i];
          if (snap === undefined) continue;
          running[i] = (running[i] ?? 0) + (snap.byCategory.get(cat.key) ?? 0);
        }
        const upperY = running.map((v) => yOfBytes(v));
        const topPts = smaps
          .map(
            (s, i) =>
              `${xOfTs(s.ts).toFixed(1)},${(upperY[i] ?? CHART_BOTTOM).toFixed(1)}`,
          )
          .join(' L');
        const botPts = smaps
          .map((s, i) => ({
            x: xOfTs(s.ts).toFixed(1),
            y: (lowerY[i] ?? CHART_BOTTOM).toFixed(1),
          }))
          .reverse()
          .map((p) => `${p.x},${p.y}`)
          .join(' L');
        stackedAreas.push({
          key: cat.key,
          color: cat.color,
          path: `M${topPts} L${botPts} Z`,
        });
      }
    }

    // Top ridge line for smaps or fallback RSS counter curve.
    let ridgeLinePath = '';
    let fallbackAreaPath = '';
    if (smaps.length >= 2) {
      ridgeLinePath = smaps
        .map(
          (s, i) =>
            `${i === 0 ? 'M' : 'L'}${xOfTs(s.ts).toFixed(1)},${yOfBytes(s.total).toFixed(1)}`,
        )
        .join(' ');
    } else if (rssCounters.length >= 2) {
      ridgeLinePath = rssCounters
        .map(
          (p, i) =>
            `${i === 0 ? 'M' : 'L'}${xOfTs(p.ts).toFixed(1)},${yOfBytes(p.bytes).toFixed(1)}`,
        )
        .join(' ');
      const firstPt = rssCounters[0];
      const lastPt = rssCounters[rssCounters.length - 1];
      if (firstPt !== undefined && lastPt !== undefined) {
        fallbackAreaPath =
          `${ridgeLinePath} L${xOfTs(lastPt.ts).toFixed(1)},${CHART_BOTTOM} ` +
          `L${xOfTs(firstPt.ts).toFixed(1)},${CHART_BOTTOM} Z`;
      }
    }

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

    // Live delta calculation for header badge / drag curtain pill.
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

    // Pointer event handlers on the SVG surface (full-width drag + click!).
    const localX = (e: PointerEvent): number => {
      const svg = e.currentTarget as SVGSVGElement;
      const rect = svg.getBoundingClientRect();
      return Math.min(
        PAD_LEFT + plotW,
        Math.max(PAD_LEFT, e.clientX - rect.left),
      );
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
      }
    };

    const onPointerDown = (e: PointerEvent) => {
      const x = localX(e);
      startDrag('range', x, e);
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
        // Align active heap dump to the end of the dragged range if dumps exist.
        if (procDumps.length > 0 && onSelectDump !== undefined) {
          const nearestDump = nearestByTs(procDumps, tSel);
          if (nearestDump !== undefined) onSelectDump(nearestDump);
        }
        return;
      }

      // Short click: check if near a Java heap dump pin first.
      const clickX = this.dragCurrentX;
      for (const d of procDumps) {
        if (Math.abs(xOfTs(d.ts) - clickX) <= 12) {
          onSelect({sel: d.ts});
          onSelectDump?.(d);
          return;
        }
      }
      // Otherwise snap to nearest smaps snapshot or exact timestamp.
      const clickTs = tsOfX(clickX);
      const nearestSmap =
        smaps.length > 0 ? nearestByTs(smaps, clickTs) : undefined;
      const targetTs = nearestSmap?.ts ?? clickTs;
      onSelect({sel: targetTs});
      if (procDumps.length > 0 && onSelectDump !== undefined) {
        const nearestDump = nearestByTs(procDumps, targetTs);
        if (nearestDump !== undefined) onSelectDump(nearestDump);
      }
    };

    const onPointerLeave = () => {
      if (this.dragMode === undefined) {
        this.hoverX = undefined;
      }
    };

    // Hover info readout
    const hoverTs = this.hoverX !== undefined ? tsOfX(this.hoverX) : undefined;
    const hoverSnap =
      hoverTs !== undefined && smaps.length > 0
        ? nearestByTs(smaps, hoverTs)
        : undefined;
    const hoverDump =
      this.hoverX !== undefined
        ? procDumps.find((d) => Math.abs(xOfTs(d.ts) - this.hoverX!) <= 12)
        : undefined;

    const firstSmap = smaps[0];
    const lastSmap = smaps[smaps.length - 1];
    const firstDump = procDumps[0];
    const lastDump = procDumps[procDumps.length - 1];
    const canCompareFullTrace =
      (smaps.length >= 2 &&
        firstSmap !== undefined &&
        lastSmap !== undefined) ||
      (procDumps.length >= 2 &&
        firstDump !== undefined &&
        lastDump !== undefined);

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
      }
    };

    const isComparing = selection?.base !== undefined;
    const headerStatus = this.renderHeaderStatus({
      isComparing,
      selection,
      liveSmapsDelta,
      liveSelSnap,
      secOf,
      canCompareFullTrace,
      onCompareFullTrace: compareFullTrace,
      onClear: () => onSelect(undefined),
    });

    return m(
      Panel,
      {className: 'pf-memtl-hero'},
      m(Panel.Header, {
        title: 'Memory Timeline',
        subtitle:
          'Drag across the timeline to select a time range and view all ' +
          'sections below as a delta (Δ). Click any sample to pin.',
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
                  height: SVG_HEIGHT,
                  viewBox: `0 0 ${svgW} ${SVG_HEIGHT}`,
                  onpointerdown: onPointerDown,
                  onpointermove: onPointerMove,
                  onpointerup: onPointerUp,
                  onpointerleave: onPointerLeave,
                  ondblclick: () => onSelect(undefined),
                },
                [
                  // Horizontal gridlines & Y-axis byte labels
                  yTicks.map((v) => {
                    const y = yOfBytes(v);
                    return m('g', {key: `y-${v}`}, [
                      m('line.pf-memtl__gridline', {
                        x1: PAD_LEFT,
                        y1: y,
                        x2: PAD_LEFT + plotW,
                        y2: y,
                      }),
                      m(
                        'text.pf-memtl__ylabel',
                        {x: PAD_LEFT - 8, y: y + 3.5},
                        v === 0 ? '0' : formatBytes(v),
                      ),
                    ]);
                  }),

                  // Stacked smaps area curves
                  stackedAreas.map((area) =>
                    m('path.pf-memtl__area', {
                      key: area.key,
                      d: area.path,
                      fill: area.color,
                    }),
                  ),

                  // Fallback RSS counter fill when < 2 smaps
                  fallbackAreaPath !== '' &&
                    m('path.pf-memtl__fallback-area', {d: fallbackAreaPath}),

                  // Single-snapshot smaps bar when exactly 1 smaps snapshot
                  smaps.length === 1 &&
                    firstSmap !== undefined &&
                    m('rect.pf-memtl__single-bar', {
                      x: xOfTs(firstSmap.ts) - 8,
                      y: yOfBytes(firstSmap.total),
                      width: 16,
                      height: Math.max(
                        2,
                        CHART_BOTTOM - yOfBytes(firstSmap.total),
                      ),
                      rx: 3,
                    }),

                  // Top ridge line
                  ridgeLinePath !== '' &&
                    m('path.pf-memtl__ridge', {d: ridgeLinePath}),

                  // Dedicated sample-markers track backdrop
                  m('rect.pf-memtl__lane-bg', {
                    x: PAD_LEFT,
                    y: CHART_BOTTOM + 6,
                    width: plotW,
                    height: 26,
                    rx: 4,
                  }),
                  m(
                    'text.pf-memtl__lane-label',
                    {x: PAD_LEFT - 8, y: LANE_Y + 3},
                    'Samples',
                  ),

                  // Native heapprofd sample ticks (orange)
                  nativeTs.map((ts, i) => {
                    const x = xOfTs(ts);
                    return m(
                      'line.pf-memtl__native-tick',
                      {
                        key: `nat-${ts.toString()}`,
                        x1: x,
                        y1: LANE_Y + 3,
                        x2: x,
                        y2: LANE_Y + 11,
                      },
                      m('title', `heapprofd sample #${i + 1} · t=${secOf(ts)}`),
                    );
                  }),

                  // Smaps sample vertical guides + curve dots + lane dots (green)
                  smaps.map((s, i) => {
                    const x = xOfTs(s.ts);
                    const y = yOfBytes(s.total);
                    const isEnd =
                      selection !== undefined &&
                      nearestByTs(smaps, selection.sel)?.ts === s.ts;
                    const isBase =
                      selection?.base !== undefined &&
                      nearestByTs(smaps, selection.base)?.ts === s.ts &&
                      !isEnd;
                    return m('g', {key: `smap-${s.ts.toString()}`}, [
                      m('line.pf-memtl__sample-guide', {
                        x1: x,
                        y1: y,
                        x2: x,
                        y2: LANE_Y,
                      }),
                      m(
                        'circle.pf-memtl__smaps-dot',
                        {
                          cx: x,
                          cy: y,
                          r: isEnd || isBase ? 5 : 3.2,
                          class: isEnd
                            ? 'pf-memtl__smaps-dot--sel'
                            : isBase
                              ? 'pf-memtl__smaps-dot--base'
                              : undefined,
                        },
                        m(
                          'title',
                          `Smaps #${i + 1} · t=${secOf(s.ts)} · ${formatBytes(s.total)}`,
                        ),
                      ),
                      m('circle.pf-memtl__smaps-lane-dot', {
                        cx: x,
                        cy: LANE_Y - 3,
                        r: 2.5,
                      }),
                    ]);
                  }),

                  // Java heap dump markers (blue diamonds + vertical dashed line)
                  procDumps.map((d, i) => {
                    const x = xOfTs(d.ts);
                    const isActive =
                      activeDump !== null &&
                      activeDump.upid === d.upid &&
                      activeDump.ts === d.ts;
                    const heapBytes = javaHeapBytesByTs.get(d.ts);
                    const r = isActive ? 6.5 : 5;
                    const cy = LANE_Y - 1;
                    const pts = `${x},${cy - r} ${x + r},${cy} ${x},${cy + r} ${x - r},${cy}`;
                    return m(
                      'g.pf-memtl__dump-pin',
                      {
                        key: `dump-${d.ts.toString()}`,
                        class: isActive
                          ? 'pf-memtl__dump-pin--active'
                          : undefined,
                      },
                      [
                        m('line.pf-memtl__dump-guide', {
                          x1: x,
                          y1: CHART_TOP,
                          x2: x,
                          y2: cy - r,
                        }),
                        heapBytes !== undefined &&
                          smaps.length === 0 &&
                          rssCounters.length < 2 &&
                          m('rect.pf-memtl__dump-bar', {
                            x: x - 9,
                            y: yOfBytes(heapBytes),
                            width: 18,
                            height: Math.max(
                              2,
                              CHART_BOTTOM - yOfBytes(heapBytes),
                            ),
                            rx: 3,
                          }),
                        isActive &&
                          m('circle.pf-memtl__dump-halo', {
                            cx: x,
                            cy,
                            r: r + 3.5,
                          }),
                        m('polygon.pf-memtl__dump-diamond', {points: pts}),
                        m(
                          'text.pf-memtl__dump-tag',
                          {x: x + r + 4, y: cy + 3.5},
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

                  // Single-point pinned selection marker (when not in range mode)
                  !isDragRange &&
                    selection !== undefined &&
                    selection.base === undefined &&
                    m('g.pf-memtl__pin-line', [
                      m('line', {
                        x1: xOfTs(selection.sel),
                        y1: CHART_TOP - 4,
                        x2: xOfTs(selection.sel),
                        y2: CHART_BOTTOM + 32,
                      }),
                      m(
                        'text.pf-memtl__pin-label',
                        {
                          x: Math.min(
                            PAD_LEFT + plotW - 44,
                            Math.max(PAD_LEFT + 44, xOfTs(selection.sel)),
                          ),
                          y: CHART_TOP - 10,
                        },
                        `t=${secOf(selection.sel)}`,
                      ),
                    ]),

                  // Shaded Range Selection Curtain + draggable Base/End handles
                  liveRange !== undefined &&
                    m('g.pf-memtl__selection', [
                      m('rect.pf-memtl__curtain', {
                        x: liveRange.x1,
                        y: CHART_TOP - 2,
                        width: Math.max(2, liveRange.x2 - liveRange.x1),
                        height: CHART_BOTTOM - CHART_TOP + 34,
                        rx: 3,
                      }),
                      // Left (BASE) boundary line + drag handle
                      m('line.pf-memtl__edge.pf-memtl__edge--base', {
                        x1: liveRange.x1,
                        y1: CHART_TOP - 4,
                        x2: liveRange.x1,
                        y2: CHART_BOTTOM + 32,
                      }),
                      !isDragging &&
                        m('rect.pf-memtl__handle.pf-memtl__handle--base', {
                          x: liveRange.x1 - 5,
                          y: (CHART_TOP + CHART_BOTTOM) / 2 - 14,
                          width: 10,
                          height: 28,
                          rx: 3,
                          onpointerdown: (e: PointerEvent) =>
                            startDrag('resize-base', liveRange.x2, e),
                        }),
                      // Right (END) boundary line + drag handle
                      m('line.pf-memtl__edge.pf-memtl__edge--sel', {
                        x1: liveRange.x2,
                        y1: CHART_TOP - 4,
                        x2: liveRange.x2,
                        y2: CHART_BOTTOM + 32,
                      }),
                      !isDragging &&
                        m('rect.pf-memtl__handle.pf-memtl__handle--sel', {
                          x: liveRange.x2 - 5,
                          y: (CHART_TOP + CHART_BOTTOM) / 2 - 14,
                          width: 10,
                          height: 28,
                          rx: 3,
                          onpointerdown: (e: PointerEvent) =>
                            startDrag('resize-sel', liveRange.x1, e),
                        }),
                      // Floating range label above curtain
                      m(
                        'text.pf-memtl__range-label',
                        {
                          x: Math.min(
                            PAD_LEFT + plotW - 80,
                            Math.max(
                              PAD_LEFT + 80,
                              (liveRange.x1 + liveRange.x2) / 2,
                            ),
                          ),
                          y: CHART_TOP - 10,
                        },
                        liveBaseTs !== undefined && liveSelTs !== undefined
                          ? `${secOf(liveBaseTs)} → ${secOf(liveSelTs)}` +
                              (liveSmapsDelta !== undefined
                                ? ` (${formatDelta(liveSmapsDelta)})`
                                : '')
                          : '',
                      ),
                    ]),

                  // Hover crosshair
                  this.hoverX !== undefined &&
                    !isDragging &&
                    m('line.pf-memtl__crosshair', {
                      x1: this.hoverX,
                      y1: CHART_TOP,
                      x2: this.hoverX,
                      y2: CHART_BOTTOM + 32,
                    }),

                  // X-axis baseline + time tick labels
                  m('line.pf-memtl__xaxis', {
                    x1: PAD_LEFT,
                    y1: AXIS_Y - 12,
                    x2: PAD_LEFT + plotW,
                    y2: AXIS_Y - 12,
                  }),
                  xTicks.map((t, i) =>
                    m(
                      'text.pf-memtl__xlabel',
                      {key: `x-${i}`, x: t.x, y: AXIS_Y + 4},
                      t.label,
                    ),
                  ),
                ],
              ),

              // Floating hover HUD pill (positioned via CSS top-right corner)
              hoverTs !== undefined &&
                !isDragging &&
                m('.pf-memtl__hud', [
                  m('span.pf-memtl__hud-ts', `t=${secOf(hoverTs)}`),
                  hoverSnap !== undefined &&
                    m(
                      'span.pf-memtl__hud-val',
                      `RSS+Swap: ${formatBytes(hoverSnap.total)}`,
                    ),
                  hoverDump !== undefined &&
                    m(
                      'span.pf-memtl__hud-dump',
                      `◆ Java Dump #${procDumps.indexOf(hoverDump) + 1}`,
                    ),
                ]),
            ]),

        // Legend + quick-select strip
        m('.pf-memtl__legend-row', [
          m('.pf-memtl__legend-items', [
            activeCategories.map((c) =>
              m('span.pf-memtl__legend-item', {key: c.key}, [
                m('span.pf-memscope-growth__swatch', {
                  style: {background: c.color},
                }),
                c.label,
              ]),
            ),
            m('.pf-memtl__legend-sep'),
            m(
              'span.pf-memtl__legend-source',
              {
                className: classNames(
                  smaps.length === 0 && 'pf-memtl__legend-source--empty',
                ),
              },
              [
                m(
                  'span.pf-memscope-capture__dot.pf-memscope-capture__dot--smaps',
                ),
                `smaps (${smaps.length})`,
              ],
            ),
            m(
              'span.pf-memtl__legend-source',
              {
                className: classNames(
                  procDumps.length === 0 && 'pf-memtl__legend-source--empty',
                ),
              },
              [
                m('span.pf-memtl__legend-diamond'),
                `java_hprof (${procDumps.length})`,
              ],
            ),
            m(
              'span.pf-memtl__legend-source',
              {
                className: classNames(
                  nativeTs.length === 0 && 'pf-memtl__legend-source--empty',
                ),
              },
              [
                m('span.pf-memtl__legend-tick'),
                `heapprofd (${nativeTs.length})`,
              ],
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
    readonly liveSmapsDelta?: number;
    readonly liveSelSnap?: SmapsSnap;
    readonly secOf: (ts: time) => string;
    readonly canCompareFullTrace: boolean;
    readonly onCompareFullTrace: () => void;
    readonly onClear: () => void;
  }): m.Children {
    const {
      isComparing,
      selection,
      liveSmapsDelta,
      liveSelSnap,
      secOf,
      canCompareFullTrace,
      onCompareFullTrace,
      onClear,
    } = opts;

    return m('.pf-memtl__status-controls', [
      isComparing && selection?.base !== undefined
        ? m('.pf-memtl__delta-badge', [
            m(Icon, {icon: 'compare_arrows'}),
            m('span', `Δ ${secOf(selection.base)} → ${secOf(selection.sel)}`),
            liveSmapsDelta !== undefined &&
              m(
                'span.pf-memtl__delta-badge-val',
                {style: {color: deltaColor(liveSmapsDelta)}},
                formatDelta(liveSmapsDelta),
              ),
          ])
        : selection !== undefined
          ? m('.pf-memtl__pin-badge', [
              m(Icon, {icon: 'photo_camera'}),
              m(
                'span',
                `Pinned @ ${secOf(selection.sel)}` +
                  (liveSelSnap !== undefined
                    ? ` · ${formatBytes(liveSelSnap.total)}`
                    : ''),
              ),
            ])
          : liveSelSnap !== undefined &&
            m('.pf-memtl__pin-badge', [
              m(Icon, {icon: 'photo_camera'}),
              m(
                'span',
                `Latest @ ${secOf(liveSelSnap.ts)} · ${formatBytes(liveSelSnap.total)}`,
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
