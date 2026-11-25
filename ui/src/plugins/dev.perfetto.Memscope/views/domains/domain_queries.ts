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

import type {Trace} from '../../../../public/trace';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../../../trace_processor/query_result';

export type SmapsCategoryKey =
  'native' | 'java' | 'file' | 'graphics' | 'stack' | 'other';

export type FileBucketKey = 'java_code' | 'so' | 'resources' | 'other_file';

export interface SmapsCategorySnapshot {
  readonly ts: bigint;
  readonly x: number; // seconds from trace start
  readonly rssSwap: number;
  readonly rss: number;
  readonly pss: number;
  readonly swap: number;
  readonly privateDirty: number;
  readonly clean: number;
  readonly byFileBucket?: ReadonlyMap<FileBucketKey, number>;
}

export interface SmapsMappingRow {
  readonly path: string;
  readonly fileBucket?: FileBucketKey;
  readonly rssSwap: number;
  readonly rss: number;
  readonly pss: number;
  readonly anon: number;
  readonly swap: number;
  readonly privateDirty: number;
  readonly clean: number;
  readonly vmaCount: number;
  readonly deltaBytes?: number;
}

export interface SmapsCategoryDetail {
  readonly snapshots: ReadonlyArray<SmapsCategorySnapshot>;
  readonly selectedTs?: bigint;
  readonly rows: ReadonlyArray<SmapsMappingRow>;
  readonly totals: {
    readonly rssSwap: number;
    readonly rss: number;
    readonly pss: number;
    readonly swap: number;
    readonly privateDirty: number;
    readonly clean: number;
    readonly vmaCount: number;
    readonly deltaBytes?: number;
  };
}

export interface CounterSeriesSummary {
  readonly name: string;
  readonly label: string;
  readonly color: string;
  readonly latest: number;
  readonly peak: number;
  readonly delta?: number;
  readonly points: ReadonlyArray<{x: number; y: number}>;
}

export interface ThreadPoolSummary {
  readonly poolName: string;
  readonly count: number;
  readonly liveCount: number;
  readonly hasMain: boolean;
  readonly sampleNames: ReadonlyArray<string>;
}

export interface ProcessThreadsData {
  readonly totalThreads: number;
  readonly liveThreads: number;
  readonly pools: ReadonlyArray<ThreadPoolSummary>;
}

export const FILE_BUCKETS: ReadonlyArray<{
  readonly key: FileBucketKey;
  readonly label: string;
  readonly shortLabel: string;
  readonly color: string;
}> = [
  {
    key: 'java_code',
    label: 'Java code (.oat/.vdex/.art/.jar)',
    shortLabel: 'Java code',
    color: '#1e8e3e',
  },
  {
    key: 'so',
    label: 'Native libs (.so)',
    shortLabel: 'Native .so',
    color: '#1a73e8',
  },
  {
    key: 'resources',
    label: 'Resources & APKs',
    shortLabel: 'Resources',
    color: '#e37400',
  },
  {
    key: 'other_file',
    label: 'Other mapped files',
    shortLabel: 'Other files',
    color: '#80868b',
  },
];

export async function loadSmapsCategoryDetail(
  trace: Trace,
  upid: number,
  category: SmapsCategoryKey,
  selectedTs?: bigint,
): Promise<SmapsCategoryDetail> {
  const startTs = trace.traceInfo.start;
  const isFile = category === 'file';

  // 1. All snapshot timestamps for this process + this category's per-snapshot totals.
  const snapRes = await trace.engine.query(`
    INCLUDE PERFETTO MODULE memscope.smaps;
    WITH all_snaps AS (
      SELECT DISTINCT ts FROM profiler_smaps WHERE upid = ${upid}
    ),
    cat_snaps AS (
      SELECT
        ts,
        CAST(ifnull(SUM(rss_kb + swap_kb), 0) * 1024 AS INT) AS rss_swap,
        CAST(ifnull(SUM(rss_kb), 0) * 1024 AS INT) AS rss,
        CAST(ifnull(SUM(proportional_resident_kb), 0) * 1024 AS INT) AS pss,
        CAST(ifnull(SUM(swap_kb), 0) * 1024 AS INT) AS swap,
        CAST(ifnull(SUM(private_dirty_kb), 0) * 1024 AS INT) AS private_dirty,
        CAST(ifnull(SUM(private_clean_resident_kb + shared_clean_resident_kb), 0) * 1024 AS INT) AS clean
      FROM profiler_smaps
      WHERE upid = ${upid}
        AND memscope_smaps_category(path) = '${category}'
      GROUP BY ts
    )
    SELECT
      a.ts AS ts,
      ifnull(c.rss_swap, 0) AS rss_swap,
      ifnull(c.rss, 0) AS rss,
      ifnull(c.pss, 0) AS pss,
      ifnull(c.swap, 0) AS swap,
      ifnull(c.private_dirty, 0) AS private_dirty,
      ifnull(c.clean, 0) AS clean
    FROM all_snaps a
    LEFT JOIN cat_snaps c ON a.ts = c.ts
    ORDER BY a.ts ASC
  `);

  const fileBucketByTs = new Map<bigint, Map<FileBucketKey, number>>();
  if (isFile) {
    const fbRes = await trace.engine.query(`
      INCLUDE PERFETTO MODULE memscope.smaps;
      SELECT ts, bucket, rss_swap_bytes AS bytes
      FROM memscope_smaps_file_breakdown
      WHERE upid = ${upid}
      ORDER BY ts ASC
    `);
    for (
      const it = fbRes.iter({ts: LONG, bucket: STR, bytes: NUM});
      it.valid();
      it.next()
    ) {
      let m = fileBucketByTs.get(it.ts);
      if (m === undefined) {
        m = new Map();
        fileBucketByTs.set(it.ts, m);
      }
      m.set(it.bucket as FileBucketKey, it.bytes);
    }
  }

  const snapshots: SmapsCategorySnapshot[] = [];
  for (
    const it = snapRes.iter({
      ts: LONG,
      rss_swap: NUM,
      rss: NUM,
      pss: NUM,
      swap: NUM,
      private_dirty: NUM,
      clean: NUM,
    });
    it.valid();
    it.next()
  ) {
    snapshots.push({
      ts: it.ts,
      x: Number(it.ts - startTs) / 1e9,
      rssSwap: it.rss_swap,
      rss: it.rss,
      pss: it.pss,
      swap: it.swap,
      privateDirty: it.private_dirty,
      clean: it.clean,
      byFileBucket: isFile ? fileBucketByTs.get(it.ts) : undefined,
    });
  }

  if (snapshots.length === 0) {
    return {
      snapshots: [],
      rows: [],
      totals: {
        rssSwap: 0,
        rss: 0,
        pss: 0,
        swap: 0,
        privateDirty: 0,
        clean: 0,
        vmaCount: 0,
      },
    };
  }

  const firstTs = snapshots[0]!.ts;
  const activeTs =
    selectedTs !== undefined && snapshots.some((s) => s.ts === selectedTs)
      ? selectedTs
      : snapshots[snapshots.length - 1]!.ts;
  const hasBaseline = snapshots.length >= 2 && activeTs !== firstTs;

  const bucketExpr = isFile ? `memscope_smaps_file_bucket(s.path)` : `''`;

  // 2. Per-path rows at activeTs (with delta vs firstTs when multiple snapshots exist).
  const rowRes = await trace.engine.query(`
    INCLUDE PERFETTO MODULE memscope.smaps;
    WITH cur AS (
      SELECT
        ifnull(s.path, '') AS path,
        ${bucketExpr} AS bucket,
        CAST(ifnull(SUM(s.rss_kb + s.swap_kb), 0) * 1024 AS INT) AS rss_swap,
        CAST(ifnull(SUM(s.rss_kb), 0) * 1024 AS INT) AS rss,
        CAST(ifnull(SUM(s.proportional_resident_kb), 0) * 1024 AS INT) AS pss,
        CAST(ifnull(SUM(s.anonymous_kb), 0) * 1024 AS INT) AS anon,
        CAST(ifnull(SUM(s.swap_kb), 0) * 1024 AS INT) AS swap,
        CAST(ifnull(SUM(s.private_dirty_kb), 0) * 1024 AS INT) AS private_dirty,
        CAST(ifnull(SUM(s.private_clean_resident_kb + s.shared_clean_resident_kb), 0) * 1024 AS INT) AS clean,
        COUNT(*) AS vma_count
      FROM profiler_smaps s
      WHERE s.upid = ${upid}
        AND s.ts = ${activeTs}
        AND memscope_smaps_category(s.path) = '${category}'
      GROUP BY s.path
    )${
      hasBaseline
        ? `,
    base AS (
      SELECT
        ifnull(s.path, '') AS path,
        CAST(ifnull(SUM(s.rss_kb + s.swap_kb), 0) * 1024 AS INT) AS rss_swap
      FROM profiler_smaps s
      WHERE s.upid = ${upid}
        AND s.ts = ${firstTs}
        AND memscope_smaps_category(s.path) = '${category}'
      GROUP BY s.path
    )
    SELECT
      cur.path,
      cur.bucket,
      cur.rss_swap,
      cur.rss,
      cur.pss,
      cur.anon,
      cur.swap,
      cur.private_dirty,
      cur.clean,
      cur.vma_count,
      cur.rss_swap - ifnull(base.rss_swap, 0) AS delta_bytes
    FROM cur
    LEFT JOIN base ON cur.path = base.path
    ORDER BY cur.rss_swap DESC`
        : `
    SELECT
      path,
      bucket,
      rss_swap,
      rss,
      pss,
      anon,
      swap,
      private_dirty,
      clean,
      vma_count,
      NULL AS delta_bytes
    FROM cur
    ORDER BY rss_swap DESC`
    }
  `);

  const rows: SmapsMappingRow[] = [];
  let totalRssSwap = 0;
  let totalRss = 0;
  let totalPss = 0;
  let totalSwap = 0;
  let totalPrivateDirty = 0;
  let totalClean = 0;
  let totalVma = 0;

  for (
    const it = rowRes.iter({
      path: STR,
      bucket: STR,
      rss_swap: NUM,
      rss: NUM,
      pss: NUM,
      anon: NUM,
      swap: NUM,
      private_dirty: NUM,
      clean: NUM,
      vma_count: NUM,
      delta_bytes: NUM_NULL,
    });
    it.valid();
    it.next()
  ) {
    rows.push({
      path: it.path || '[anonymous]',
      fileBucket: isFile ? (it.bucket as FileBucketKey) : undefined,
      rssSwap: it.rss_swap,
      rss: it.rss,
      pss: it.pss,
      anon: it.anon,
      swap: it.swap,
      privateDirty: it.private_dirty,
      clean: it.clean,
      vmaCount: it.vma_count,
      deltaBytes: it.delta_bytes ?? undefined,
    });
    totalRssSwap += it.rss_swap;
    totalRss += it.rss;
    totalPss += it.pss;
    totalSwap += it.swap;
    totalPrivateDirty += it.private_dirty;
    totalClean += it.clean;
    totalVma += it.vma_count;
  }

  const activeSnap = snapshots.find((s) => s.ts === activeTs)!;
  const deltaBytes = hasBaseline
    ? activeSnap.rssSwap - snapshots[0]!.rssSwap
    : undefined;

  return {
    snapshots,
    selectedTs: activeTs,
    rows,
    totals: {
      rssSwap: totalRssSwap,
      rss: totalRss,
      pss: totalPss,
      swap: totalSwap,
      privateDirty: totalPrivateDirty,
      clean: totalClean,
      vmaCount: totalVma,
      deltaBytes,
    },
  };
}

const GFX_COUNTERS: ReadonlyArray<{
  readonly name: string;
  readonly label: string;
  readonly color: string;
}> = [
  {name: 'mem.dmabuf_rss', label: 'DMA-BUF RSS', color: '#a142f4'},
  {name: 'gpu_memory', label: 'GPU Memory', color: '#7c3aed'},
  {name: 'mem.rss.shmem', label: 'Shared Memory (Shmem)', color: '#c084fc'},
];

export async function loadGraphicsCounters(
  trace: Trace,
  upid: number,
): Promise<ReadonlyArray<CounterSeriesSummary>> {
  const startTs = trace.traceInfo.start;
  const namesSql = GFX_COUNTERS.map((c) => `'${c.name}'`).join(', ');
  const res = await trace.engine.query(`
    SELECT t.name AS name, c.ts AS ts, c.value AS value
    FROM counter c
    JOIN process_counter_track t ON c.track_id = t.id
    WHERE t.upid = ${upid} AND t.name IN (${namesSql})
    ORDER BY c.ts ASC
  `);

  const byName = new Map<string, Array<{x: number; y: number}>>();
  for (
    const it = res.iter({name: STR, ts: LONG, value: NUM});
    it.valid();
    it.next()
  ) {
    let arr = byName.get(it.name);
    if (arr === undefined) {
      arr = [];
      byName.set(it.name, arr);
    }
    arr.push({x: Number(it.ts - startTs) / 1e9, y: it.value});
  }

  const out: CounterSeriesSummary[] = [];
  for (const meta of GFX_COUNTERS) {
    const pts = byName.get(meta.name);
    if (pts === undefined || pts.length === 0) continue;
    const peak = Math.max(...pts.map((p) => p.y));
    const latest = pts[pts.length - 1]!.y;
    if (peak <= 0 && latest <= 0) continue;
    out.push({
      name: meta.name,
      label: meta.label,
      color: meta.color,
      latest,
      peak,
      delta: pts.length >= 2 ? latest - pts[0]!.y : undefined,
      points: pts,
    });
  }
  return out;
}

function normalizeThreadPoolName(raw: string): string {
  const trimmed = raw.trim();
  if (trimmed === '') return '<unnamed>';
  // Collapse pool-3-thread-12 -> pool-*-thread-*, Binder:4242_1 -> Binder:*,
  // AsyncTask #4 -> AsyncTask #*, worker-12 -> worker-*
  return trimmed
    .replace(/^Binder:[\d_A-Fa-f]+$/, 'Binder:*')
    .replace(/pool-\d+-thread-\d+/gi, 'pool-*-thread-*')
    .replace(/(#\d+)$/, '#*')
    .replace(/([-_:\s])\d+$/, '$1*')
    .replace(/\d+$/, '*');
}

export async function loadProcessThreads(
  trace: Trace,
  upid: number,
): Promise<ProcessThreadsData> {
  const res = await trace.engine.query(`
    SELECT
      tid,
      name,
      ifnull(is_main_thread, 0) AS is_main,
      end_ts
    FROM thread
    WHERE upid = ${upid}
    ORDER BY is_main DESC, tid ASC
  `);

  const groups = new Map<
    string,
    {count: number; liveCount: number; hasMain: boolean; samples: Set<string>}
  >();
  let totalThreads = 0;
  let liveThreads = 0;

  for (
    const it = res.iter({
      tid: NUM,
      name: STR_NULL,
      is_main: NUM,
      end_ts: NUM_NULL,
    });
    it.valid();
    it.next()
  ) {
    totalThreads++;
    const isLive = it.end_ts === null;
    if (isLive) liveThreads++;
    const rawName = it.name ?? `tid-${it.tid}`;
    const pool = it.is_main !== 0 ? rawName : normalizeThreadPoolName(rawName);
    let g = groups.get(pool);
    if (g === undefined) {
      g = {count: 0, liveCount: 0, hasMain: false, samples: new Set()};
      groups.set(pool, g);
    }
    g.count++;
    if (isLive) g.liveCount++;
    if (it.is_main !== 0) g.hasMain = true;
    if (g.samples.size < 3) g.samples.add(rawName);
  }

  const pools: ThreadPoolSummary[] = Array.from(groups.entries())
    .map(([poolName, g]) => ({
      poolName,
      count: g.count,
      liveCount: g.liveCount,
      hasMain: g.hasMain,
      sampleNames: Array.from(g.samples),
    }))
    .sort((a, b) => b.count - a.count || a.poolName.localeCompare(b.poolName));

  return {totalThreads, liveThreads, pools};
}
