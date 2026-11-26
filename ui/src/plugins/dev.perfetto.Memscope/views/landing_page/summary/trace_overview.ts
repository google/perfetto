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

// The top-level "trace overview" billboard: headline memory stat cards for one
// process (Peak / Anon RSS + spike, memory Δ + trend, and DMA Buffer RSS when
// present). Uptime and OOM score live in the top process header bar beside the
// process selector.

import m from 'mithril';
import {AsyncMemo} from '../../../../../base/async_memo';
import {Time, type time} from '../../../../../base/time';
import type {Trace} from '../../../../../public/trace';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
} from '../../../../../trace_processor/query_result';
import {BillboardStrip} from '../../../components/billboard';
import {deltaText, formatBytes, formatDelta, statCard} from '../mem_format';

// Process memory stats from the last heap-graph dump (useful on heap-dump-only
// traces without smaps, and for DMA-BUF RSS).
interface DumpMemStats {
  readonly anonRssAndSwapSize?: number;
  readonly dmabufRssSize?: number;
}

// RSS counter maxima over the whole trace.
interface CounterData {
  readonly rssWatermark?: number; // max of mem.rss.watermark
  readonly rssMax?: number; // max of mem.rss
}

// Per-snapshot total anon+swap (smaps), ts-sorted across the whole trace.
type AnonSwapSeries = readonly {ts: time; total: number}[];

async function loadDumpMemStats(
  trace: Trace,
  upid: number,
): Promise<DumpMemStats> {
  try {
    await trace.engine.query(
      'INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_stats;',
    );
    const res = await trace.engine.query(`
      SELECT
        s.anon_rss_and_swap_size AS anon_rss_and_swap_size,
        s.dmabuf_rss_size AS dmabuf_rss_size
      FROM android_heap_graph_stats s
      WHERE s.upid = ${upid}
      ORDER BY s.graph_sample_ts DESC
      LIMIT 1
    `);
    const it = res.iter({
      anon_rss_and_swap_size: NUM_NULL,
      dmabuf_rss_size: NUM_NULL,
    });
    if (!it.valid()) return {};
    return {
      anonRssAndSwapSize: it.anon_rss_and_swap_size ?? undefined,
      dmabufRssSize: it.dmabuf_rss_size ?? undefined,
    };
  } catch {
    return {};
  }
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

// One stat slot: the value when known, `undefined` once loaded but absent
// (→ "-"), and a `loading` flag while the underlying query is in flight
// (→ "…"). `sub` is an optional caption shown under an available value.
interface Cell {
  readonly label: string;
  readonly loading: boolean;
  readonly value?: m.Children;
  readonly sub?: m.Children;
}

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

function cellCard(cells: Cell[]): m.Child {
  return statCard(cells.map(renderCell));
}

export interface TraceOverviewAttrs {
  readonly trace: Trace;
  readonly upid: number;
}

export class TraceOverview implements m.ClassComponent<TraceOverviewAttrs> {
  private readonly dumpStatsSlot = new AsyncMemo<DumpMemStats>();
  private readonly counterSlot = new AsyncMemo<CounterData>();
  private readonly smapsSlot = new AsyncMemo<AnonSwapSeries>();

  onremove() {
    this.dumpStatsSlot.dispose();
    this.counterSlot.dispose();
    this.smapsSlot.dispose();
  }

  view({attrs}: m.Vnode<TraceOverviewAttrs>): m.Children {
    const {trace, upid} = attrs;
    const key = {traceId: trace.traceInfo.uuid, upid};
    const dumpStats = this.dumpStatsSlot.use({
      key,
      compute: () => loadDumpMemStats(trace, upid),
    }).data;
    const counters = this.counterSlot.use({
      key,
      compute: () => loadCounters(trace, upid),
    }).data;
    const snaps = this.smapsSlot.use({
      key,
      compute: () => loadAnonSwap(trace, upid),
    }).data;

    const dumpStatsLoading = dumpStats === undefined;
    const countersLoading = counters === undefined;
    const snapsLoading = snaps === undefined;

    // Card 1: Peak RSS (smaps anon+swap, falling back to counter-backed
    // anon_rss_and_swap_size at dump time when smaps is absent) + RSS spike.
    const smapsPeak =
      snaps !== undefined && snaps.length > 0
        ? Math.max(...snaps.map((r) => r.total))
        : undefined;
    const fallbackAnon = dumpStats?.anonRssAndSwapSize;
    const peak = smapsPeak ?? fallbackAnon;
    const peakLoading =
      snapsLoading || (smapsPeak === undefined && dumpStatsLoading);
    const wm = counters?.rssWatermark;
    const rssMax = counters?.rssMax;
    const spike =
      wm !== undefined && rssMax !== undefined
        ? Math.max(0, wm - rssMax)
        : undefined;
    const rssCard = cellCard([
      {
        label: smapsPeak !== undefined ? 'Peak RSS' : 'Anon RSS + Swap',
        loading: peakLoading,
        value: peak !== undefined ? formatBytes(peak) : undefined,
        sub: smapsPeak !== undefined ? 'rss anon + swap' : 'at heap dump',
      },
      {
        label: 'RSS spike',
        loading: countersLoading,
        value: spike !== undefined ? deltaText(spike) : undefined,
        sub: 'hi-watermark − max',
      },
    ]);

    // Card 2: Memory Δ + trend (smaps, first → last snapshot).
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

    // Card 3 (optional): DMA Buffer RSS when captured in heap-graph stats.
    const dmabufCard =
      dumpStats?.dmabufRssSize !== undefined
        ? cellCard([
            {
              label: 'DMA Buffer RSS',
              loading: false,
              value: formatBytes(dumpStats.dmabufRssSize),
              sub: 'at heap dump',
            },
          ])
        : undefined;

    return m(BillboardStrip, [rssCard, deltaCard, dmabufCard]);
  }
}
