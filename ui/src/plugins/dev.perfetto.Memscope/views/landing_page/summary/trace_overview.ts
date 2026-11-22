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
import {AsyncMemo} from '../../../../../base/async_memo';
import type {Trace} from '../../../../../public/trace';
import {NUM_NULL} from '../../../../../trace_processor/query_result';
import {deltaText, formatBytes, formatDelta, statCard} from '../mem_format';
import {BillboardStrip} from '../../../components/billboard';
import {ColorChip} from '../../../components/color_chip';
import {oomScoreBucket, oomBucketLabel} from '../../../process_data';

// Uptime + OOM score, from the process's last heap-graph dump.
interface StatsData {
  readonly uptime?: number; // ns, at the last heap dump
  readonly oomScoreAdj?: number; // at the last heap dump
}

// RSS spike: watermark overshoot above the best-sampled RSS.
interface CounterData {
  readonly spike?: number;
}

// Anon + swap (smaps) over the whole trace: peak, and the change from the
// first to the last snapshot.
interface TrendData {
  readonly peak?: number;
  readonly delta?: number;
  readonly perHour?: number;
}

async function loadStats(trace: Trace, upid: number): Promise<StatsData> {
  const res = await trace.engine.query(`
    INCLUDE PERFETTO MODULE memscope.summary;
    SELECT uptime, oom_score_adj
    FROM memscope_process_last_heap_dump
    WHERE upid = ${upid}
  `);
  const it = res.iter({uptime: NUM_NULL, oom_score_adj: NUM_NULL});
  if (!it.valid()) return {};
  return {
    uptime: it.uptime ?? undefined,
    oomScoreAdj: it.oom_score_adj ?? undefined,
  };
}

async function loadCounters(trace: Trace, upid: number): Promise<CounterData> {
  const res = await trace.engine.query(`
    INCLUDE PERFETTO MODULE memscope.summary;
    SELECT rss_spike_bytes AS spike
    FROM memscope_process_rss_counters
    WHERE upid = ${upid}
  `);
  const it = res.iter({spike: NUM_NULL});
  return {spike: it.valid() ? (it.spike ?? undefined) : undefined};
}

async function loadTrend(trace: Trace, upid: number): Promise<TrendData> {
  const res = await trace.engine.query(`
    INCLUDE PERFETTO MODULE memscope.summary;
    SELECT
      peak_bytes AS peak,
      delta_bytes AS delta,
      trend_bytes_per_hour AS per_hour
    FROM memscope_process_memory_trend
    WHERE upid = ${upid}
  `);
  const it = res.iter({peak: NUM_NULL, delta: NUM_NULL, per_hour: NUM_NULL});
  if (!it.valid()) return {};
  return {
    peak: it.peak ?? undefined,
    delta: it.delta ?? undefined,
    perHour: it.per_hour ?? undefined,
  };
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
// the loading/absent states to "…" / "N/A - not available".
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

// A card whose stats are all Cells - always rendered, so missing facts surface
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
  private readonly statsSlot = new AsyncMemo<StatsData>();
  private readonly counterSlot = new AsyncMemo<CounterData>();
  private readonly smapsSlot = new AsyncMemo<TrendData>();

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
      compute: () => loadStats(trace, upid),
    }).data;
    const counters = this.counterSlot.use({
      key,
      compute: () => loadCounters(trace, upid),
    }).data;
    const snaps = this.smapsSlot.use({
      key,
      compute: () => loadTrend(trace, upid),
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
    const peak = snaps?.peak;
    const spike = counters?.spike;
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
    const total = snaps?.delta;
    const perHour = snaps?.perHour;
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
          total !== undefined && perHour !== undefined
            ? deltaText(total, `${formatDelta(perHour)}/h`)
            : undefined,
      },
    ]);

    // GC / Java allocation churn: not yet wired to a data source (needs ART GC
    // events + allocation-rate sampling). Hidden until then.
    // const churnCard = cellCard([
    //   {label: 'GC churn', loading: false, value: undefined},
    //   {label: 'Java churn', loading: false, value: undefined},
    // ]);

    return m(BillboardStrip, [uptimeCard, rssCard, deltaCard]);
  }
}
