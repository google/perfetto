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

// The "Composition over time" line graph: resident memory by smaps category,
// one stacked point per snapshot, for one process across the whole trace. Owns
// its own query slot, loading logic and (currently internal) snapshot
// selection.

import m from 'mithril';
import {QuerySlot} from '../../../../../base/query_slot';
import {Time, type time} from '../../../../../base/time';
import {
  LineChartSvg,
  type LineChartData,
} from '../../../../../components/widgets/charts_svg/line_chart_svg';
import type {Trace} from '../../../../../public/trace';
import {LONG, NUM, STR} from '../../../../../trace_processor/query_result';
import {Icon} from '../../../../../widgets/icon';
import {Panel} from '../../../components/panel';
import {niceKbInterval} from '../../../utils';
import {deltaText, formatBytes} from '../mem_format';
import {type MemSelection, nearestByTs} from '../selection';
import {emptyPanel, loadingPanel} from '../section_widgets';
import {SMAPS_CATEGORIES, SMAPS_CATEGORY_CASE_SQL} from '../smaps_categories';

// One smaps snapshot (a single ts): per-category resident+swap bytes and the
// total footprint. The x position (seconds from trace start) is precomputed.
interface TimelineSnapshot {
  readonly ts: time;
  readonly x: number; // seconds from trace start
  readonly total: number;
  readonly byCategory: ReadonlyMap<string, number>; // category key → rss+swap
}

interface TimelineData {
  readonly snapshots: TimelineSnapshot[];
  readonly chart?: LineChartData;
}

async function loadTimelineData(
  trace: Trace,
  upid: number,
): Promise<TimelineData> {
  const byTs = new Map<bigint, Map<string, number>>();
  const res = await trace.engine.query(`
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
    const it = res.iter({ts: LONG, category: STR, bytes: NUM});
    it.valid();
    it.next()
  ) {
    let cats = byTs.get(it.ts);
    if (cats === undefined) {
      cats = new Map();
      byTs.set(it.ts, cats);
    }
    cats.set(it.category, (cats.get(it.category) ?? 0) + it.bytes);
  }

  const start = trace.traceInfo.start;
  const snapshots: TimelineSnapshot[] = Array.from(byTs.entries()).map(
    ([ts, byCategory]) => {
      let total = 0;
      for (const v of byCategory.values()) total += v;
      return {
        ts: Time.fromRaw(ts),
        x: Number(ts - start) / 1e9,
        total,
        byCategory,
      };
    },
  );

  // Stacked composition chart: one series per category, dropping categories
  // that are zero everywhere.
  let chart: LineChartData | undefined;
  if (snapshots.length > 0) {
    const series = SMAPS_CATEGORIES.map((c) => ({
      name: c.label,
      color: c.color,
      points: snapshots.map((s) => ({x: s.x, y: s.byCategory.get(c.key) ?? 0})),
    })).filter((s) => s.points.some((p) => p.y > 0));
    chart = {series};
  }

  return {snapshots, chart};
}

export interface CompositionTimelineAttrs {
  readonly trace: Trace;
  readonly upid: number;
  // The page-wide selection (by ts), or undefined when nothing is selected
  // yet (the timeline then highlights the latest snapshot).
  readonly selection?: MemSelection;
  // Called when the user clicks a snapshot or brushes a range.
  readonly onSelect: (selection: MemSelection) => void;
  // Optional content rendered inside the panel, below the chart (e.g. the
  // whole-trace "where the growth went" bar).
  readonly belowChart?: m.Children;
}

export class CompositionTimeline
  implements m.ClassComponent<CompositionTimelineAttrs>
{
  private readonly slot = new QuerySlot<TimelineData>();

  onremove() {
    this.slot.dispose();
  }

  view({attrs}: m.Vnode<CompositionTimelineAttrs>): m.Children {
    const {trace, upid, selection, onSelect, belowChart} = attrs;
    const data = this.slot.use({
      key: {traceId: trace.traceInfo.uuid, upid},
      queryFn: () => loadTimelineData(trace, upid),
    }).data;
    if (data === undefined) {
      return loadingPanel({title: 'Composition over time'}); // Still loading.
    }

    const snaps = data.snapshots;
    if (snaps.length === 0 || data.chart === undefined) {
      return emptyPanel({
        title: 'Composition over time',
        message: 'No smaps snapshots in this trace for this process.',
      });
    }

    // Resolve the page selection (by ts) to snapshot indices for rendering.
    // No selection → highlight the latest snapshot.
    const lastIdx = snaps.length - 1;
    const idxOfTs = (ts: time) => snaps.indexOf(nearestByTs(snaps, ts)!);
    const selIdx = selection !== undefined ? idxOfTs(selection.sel) : lastIdx;
    let baseIdx =
      selection?.base !== undefined ? idxOfTs(selection.base) : undefined;
    if (baseIdx === selIdx) baseIdx = undefined;
    const snap = snaps[selIdx];
    const base = baseIdx !== undefined ? snaps[baseIdx] : undefined;

    // Index of the snapshot whose x is closest to a clicked/brushed time.
    const nearestSnap = (x: number) => {
      let bestIdx = 0;
      for (let i = 1; i < snaps.length; i++) {
        if (Math.abs(snaps[i].x - x) < Math.abs(snaps[bestIdx].x - x)) {
          bestIdx = i;
        }
      }
      return bestIdx;
    };

    return m(
      Panel,
      m(Panel.Header, {
        title: 'Composition over time',
        subtitle:
          'Resident memory by region, one point per smaps snapshot. Click ' +
          'a snapshot to inspect it, or drag to compare two.',
        controls: m('span.pf-memscope-memmap__badge', [
          m(Icon, {icon: 'photo_camera'}),
          base !== undefined && baseIdx !== undefined
            ? [
                `Snapshot #${baseIdx + 1} → #${selIdx + 1} · `,
                deltaText(snap.total - base.total),
              ]
            : `Snapshot #${selIdx + 1} · ${formatBytes(snap.total)}`,
        ]),
      }),
      m(
        Panel.Body,
        m('.pf-memscope-memmap', [
          m(LineChartSvg, {
            data: data.chart,
            height: 160,
            stacked: true,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Size',
            showLegend: true,
            showPoints: true,
            gridLines: 'vertical',
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => formatBytes(v),
            // Snap ticks to power-of-2 (MiB) boundaries so the IEC labels read as
            // clean MiB/GiB values rather than fractional decimals.
            // This axis is in bytes; niceKbInterval works in KB, so scale in
            // and back out by 1024 to get a power-of-2-aligned byte interval.
            yAxisMinInterval:
              niceKbInterval(Math.max(0, ...snaps.map((s) => s.total)) / 1024) *
              1024,
            markers: [
              // In compare mode, show the baseline snapshot as a muted marker so
              // it's distinguishable from the (accent-coloured) selected one.
              base !== undefined &&
                baseIdx !== undefined && {
                  x: base.x,
                  label: `#${baseIdx + 1}`,
                  color: 'var(--pf-color-text-muted)',
                },
              // Selected snapshot stays red (the marker default).
              {x: snap.x, label: `#${selIdx + 1}`},
            ].filter(Boolean) as ReadonlyArray<{
              x: number;
              label?: string;
              color?: string;
            }>,
            selection:
              base !== undefined ? {start: base.x, end: snap.x} : undefined,
            onPointClick: (x: number) => {
              onSelect({sel: snaps[nearestSnap(x)].ts});
            },
            onBrush: ({start, end}: {start: number; end: number}) => {
              const a = nearestSnap(start);
              const b = nearestSnap(end);
              onSelect(
                a === b
                  ? {sel: snaps[a].ts}
                  : {sel: snaps[b].ts, base: snaps[a].ts},
              );
            },
          }),
          belowChart,
          m(
            '.pf-memscope-memmap__snaps',
            snaps.map((s, i) =>
              m(
                'button.pf-memscope-memmap__snap',
                {
                  className:
                    i === selIdx
                      ? 'pf-memscope-memmap__snap--active'
                      : i === baseIdx
                        ? 'pf-memscope-memmap__snap--base'
                        : undefined,
                  title: `t=${s.x.toFixed(1)}s · ${formatBytes(s.total)}`,
                  onclick: () => onSelect({sel: s.ts}),
                },
                `#${i + 1}`,
              ),
            ),
          ),
        ]),
      ),
    );
  }
}
