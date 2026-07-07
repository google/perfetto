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

// The top-level "trace overview" billboard: a row of headline stat cards for
// one process (uptime / OOM score, peak RSS + spike, memory Δ + trend). Each
// underlying query has its own slot so they load independently, and every card
// is always rendered: missing facts show "N/A" rather than dropping the card.
// Spans the whole trace, independent of the page's snapshot selection.

import m from 'mithril';
import {QuerySlot} from '../../../../../base/query_slot';
import {Time, type time} from '../../../../../base/time';
import type {Trace} from '../../../../../public/trace';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
} from '../../../../../trace_processor/query_result';
import {deltaText, formatBytes, formatDelta, statCard} from '../mem_format';
import {BillboardStrip} from '../../../components/billboard';
import {ColorChip} from '../../../components/color_chip';
import {oomScoreBucket, oomBucketLabel} from '../../../process_data';

// Uptime + OOM score, from the process's last heap-graph dump.
interface StatsData {
  readonly uptime?: number; // ns, at the last heap dump
  readonly oomScoreAdj?: number; // at the last heap dump
}

// RSS counter maxima over the whole trace.
interface CounterData {
  readonly rssWatermark?: number; // max of mem.rss.watermark
  readonly rssMax?: number; // max of mem.rss
}

// Per-snapshot total anon+swap (smaps), ts-sorted across the whole trace.
type AnonSwapSeries = readonly {ts: time; total: number}[];

async function loadStats(trace: Trace, upid: number): Promise<StatsData> {
  await trace.engine.query(
    'INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_stats;',
  );
  const res = await trace.engine.query(`
    SELECT
      s.process_uptime AS uptime,
      s.oom_score_adj AS oom_score_adj
    FROM android_heap_graph_stats s
    WHERE s.upid = ${upid}
    ORDER BY s.graph_sample_ts DESC
    LIMIT 1
  `);
  const it = res.iter({uptime: NUM_NULL, oom_score_adj: NUM_NULL});
  if (!it.valid()) return {};
  return {
    uptime: it.uptime ?? undefined,
    oomScoreAdj: it.oom_score_adj ?? undefined,
  };
}

async function loadCounters(trace: Trace, upid: number): Promise<CounterData> {
  let rssWatermark: number | undefined;
  let rssMax: number | undefined;
  const res = await trace.engine.query(`
    SELECT t.name AS counter_name, MAX(c.value) AS max_value
    FROM counter c
    JOIN process_counter_track t ON c.track_id = t.id
    WHERE t.upid = ${upid}
      AND t.name IN ('mem.rss.watermark', 'mem.rss')
    GROUP BY t.name
  `);
  for (
    const it = res.iter({counter_name: STR, max_value: NUM});
    it.valid();
    it.next()
  ) {
    if (it.counter_name === 'mem.rss.watermark') rssWatermark = it.max_value;
    else if (it.counter_name === 'mem.rss') rssMax = it.max_value;
  }
  return {rssWatermark, rssMax};
}

async function loadAnonSwap(
  trace: Trace,
  upid: number,
): Promise<AnonSwapSeries> {
  const anonSwap: {ts: time; total: number}[] = [];
  const res = await trace.engine.query(`
    SELECT
      s.ts AS ts,
      CAST(ifnull(SUM(s.anonymous_kb + s.swap_kb), 0) * 1024 AS INT) AS total
    FROM profiler_smaps s
    WHERE s.upid = ${upid}
    GROUP BY s.ts
    ORDER BY s.ts ASC
  `);
  for (const it = res.iter({ts: LONG, total: NUM}); it.valid(); it.next()) {
    anonSwap.push({ts: Time.fromRaw(it.ts), total: it.total});
  }
  return anonSwap;
}

// "4h 12m" / "5m 3s" / "37s" from a duration in nanoseconds.
function formatDurationShort(ns: number): string {
  const secs = ns / 1e9;
  if (secs < 60) return `${secs.toFixed(0)}s`;
  const mins = secs / 60;
  if (mins < 60) return `${Math.floor(mins)}m ${Math.round(secs % 60)}s`;
  return `${Math.floor(mins / 60)}h ${Math.round(mins % 60)}m`;
}

// One stat slot: the value when known, `undefined` once loaded but absent
// (→ "N/A"), and a `loading` flag while the underlying query is in flight
// (→ "…"). `sub` is an optional caption shown under an available value.
interface Cell {
  readonly label: string;
  readonly loading: boolean;
  readonly value?: m.Children;
  readonly sub?: m.Children;
}

// Renders a cell into the {label, value, sub} shape statCard expects, mapping
// the loading/absent states to "…" / "N/A — not available".
function renderCell(c: Cell): {
  label: string;
  value: m.Children;
  sub?: m.Children;
} {
  if (c.loading) return {label: c.label, value: '…'};
  if (c.value === undefined) {
    return {label: c.label, value: '-', sub: 'not available'};
  }
  return {label: c.label, value: c.value, sub: c.sub};
}

// A card whose stats are all Cells — always rendered, so missing facts surface
// as "N/A" rather than dropping the whole card.
function cellCard(cells: Cell[]): m.Child {
  return statCard(cells.map(renderCell));
}

export interface TraceOverviewAttrs {
  readonly trace: Trace;
  readonly upid: number;
}

export class TraceOverview implements m.ClassComponent<TraceOverviewAttrs> {
  // One slot per query, so the three load and redraw independently.
  private readonly statsSlot = new QuerySlot<StatsData>();
  private readonly counterSlot = new QuerySlot<CounterData>();
  private readonly smapsSlot = new QuerySlot<AnonSwapSeries>();

  onremove() {
    this.statsSlot.dispose();
    this.counterSlot.dispose();
    this.smapsSlot.dispose();
  }

  view({attrs}: m.Vnode<TraceOverviewAttrs>): m.Children {
    const {trace, upid} = attrs;
    const key = {traceId: trace.traceInfo.uuid, upid};
    const stats = this.statsSlot.use({
      key,
      queryFn: () => loadStats(trace, upid),
    }).data;
    const counters = this.counterSlot.use({
      key,
      queryFn: () => loadCounters(trace, upid),
    }).data;
    const snaps = this.smapsSlot.use({
      key,
      queryFn: () => loadAnonSwap(trace, upid),
    }).data;

    const statsLoading = stats === undefined;
    const countersLoading = counters === undefined;
    const snapsLoading = snaps === undefined;

    // Card 1: uptime + OOM score (heap-graph dump). The OOM chip reuses the
    // same ColorChip + bucket palette as the dashboard process table.
    const oomBucket =
      stats?.oomScoreAdj !== undefined
        ? oomScoreBucket(stats.oomScoreAdj)
        : undefined;
    const uptimeCard = cellCard([
      {
        label: 'Uptime',
        loading: statsLoading,
        value:
          stats?.uptime !== undefined
            ? formatDurationShort(stats.uptime)
            : undefined,
      },
      {
        label: 'OOM score',
        loading: statsLoading,
        value:
          stats?.oomScoreAdj !== undefined ? `${stats.oomScoreAdj}` : undefined,
        sub:
          oomBucket !== undefined
            ? m(ColorChip, {color: oomBucket.color}, oomBucketLabel(oomBucket))
            : undefined,
      },
    ]);

    // Card 2: peak RSS (smaps) + RSS spike (counters).
    const peak =
      snaps !== undefined && snaps.length > 0
        ? Math.max(...snaps.map((r) => r.total))
        : undefined;
    const wm = counters?.rssWatermark;
    const rssMax = counters?.rssMax;
    // Spike is the watermark's overshoot above the best-sampled RSS. When the
    // poll caught the peak (wm == rssMax) that's a real "no spike" → 0, not
    // missing data; only an absent counter leaves it undefined.
    const spike =
      wm !== undefined && rssMax !== undefined
        ? Math.max(0, wm - rssMax)
        : undefined;
    const rssCard = cellCard([
      {
        label: 'Peak RSS',
        loading: snapsLoading,
        value: peak !== undefined ? formatBytes(peak) : undefined,
        sub: 'rss anon + swap',
      },
      {
        label: 'RSS spike',
        loading: countersLoading,
        value: spike !== undefined ? deltaText(spike) : undefined,
        sub: 'hi-watermark − max',
      },
    ]);

    // Card 3: memory Δ + trend (smaps, first → last snapshot).
    const haveDelta = snaps !== undefined && snaps.length > 1;
    const first = haveDelta ? snaps[0] : undefined;
    const last = haveDelta ? snaps[snaps.length - 1] : undefined;
    const total =
      first !== undefined && last !== undefined
        ? last.total - first.total
        : undefined;
    const spanSeconds =
      first !== undefined && last !== undefined
        ? Number(last.ts - first.ts) / 1e9
        : 0;
    const deltaCard = cellCard([
      {
        label: 'Memory Δ',
        loading: snapsLoading,
        value: total !== undefined ? deltaText(total) : undefined,
        sub: 'first → last',
      },
      {
        label: 'Trend',
        loading: snapsLoading,
        value:
          total !== undefined && spanSeconds > 0
            ? deltaText(total, `${formatDelta((total / spanSeconds) * 3600)}/h`)
            : undefined,
      },
    ]);

    // GC / Java allocation churn: not yet wired to a data source (needs ART GC
    // events + allocation-rate sampling), so both cells report "not available"
    // rather than placeholder numbers.
    const churnCard = cellCard([
      {label: 'GC churn', loading: false, value: undefined},
      {label: 'Java churn', loading: false, value: undefined},
    ]);

    return m(BillboardStrip, [uptimeCard, rssCard, deltaCard, churnCard]);
  }
}
