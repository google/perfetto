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

import {formatBytesIec} from '../../../../base/bytes_format';
import {Time, type time} from '../../../../base/time';
import type {Trace} from '../../../../public/trace';
import {
  LONG,
  LONG_NULL,
  NUM,
  STR,
  STR_NULL,
} from '../../../../trace_processor/query_result';

export const ALL_HEAPS = '__all__';

export function fmtNativeBytes(bytes: number): string {
  return formatBytesIec(Math.max(0, bytes));
}

export function fmtNativeDelta(bytes: number): string {
  const sign = bytes > 0 ? '+' : bytes < 0 ? '-' : '';
  return `${sign}${formatBytesIec(Math.abs(bytes))}`;
}

// SQL predicate matching low-level allocator wrapper frames that should be
// skipped when attributing a callsite to a blaming library & function.
const IS_ALLOCATOR_FRAME_SQL = `
  CASE
    WHEN f.name IN (
      'malloc', 'calloc', 'realloc', 'free', 'memalign', 'posix_memalign',
      'aligned_alloc', 'pvalloc', 'valloc', 'strdup', 'strndup',
      'libc_malloc', 'libc_calloc', 'libc_realloc',
      'je_malloc', 'je_calloc', 'je_realloc',
      'scudo_malloc', 'scudo_calloc', 'scudo_realloc',
      '__libc_malloc', '__libc_calloc', '__libc_realloc',
      '_Znwm', '_Znam', '_Znwj', '_Znaj',
      '_ZnwmRKSt9nothrow_t', '_ZnamRKSt9nothrow_t',
      '_ZnwmSt11align_val_t', '_ZnamSt11align_val_t'
    ) THEN 1
    WHEN coalesce(f.deobfuscated_name, f.name, '') LIKE 'operator new%' THEN 1
    WHEN coalesce(f.deobfuscated_name, f.name, '') LIKE 'scudo::%' THEN 1
    WHEN coalesce(f.deobfuscated_name, f.name, '') LIKE 'std::__1::__libcpp_allocate%' THEN 1
    WHEN coalesce(f.deobfuscated_name, f.name, '') LIKE 'std::__1::allocator%' THEN 1
    WHEN coalesce(mp.name, '') LIKE '%/libc.so'
      AND (
        lower(coalesce(f.deobfuscated_name, f.name, '')) LIKE '%alloc%'
        OR lower(coalesce(f.deobfuscated_name, f.name, '')) LIKE '%scudo%'
      ) THEN 1
    ELSE 0
  END
`;

// Builds a CTE `attributed_site(callsite_id, attr_frame, attr_mapping)` that
// resolves each distinct callsite matching `where` to its first non-allocator
// frame walking up `stack_profile_callsite.parent_id` (falling back to leaf).
function attributedSitesCte(where: string): string {
  return `
    target_sites AS (
      SELECT DISTINCT a.callsite_id
      FROM heap_profile_allocation a
      WHERE ${where}
    ),
    walk(root_cid, cid, depth, is_alloc) AS (
      SELECT
        ts.callsite_id,
        sc.id,
        0,
        (${IS_ALLOCATOR_FRAME_SQL})
      FROM target_sites ts
      JOIN stack_profile_callsite sc ON sc.id = ts.callsite_id
      JOIN stack_profile_frame f ON f.id = sc.frame_id
      LEFT JOIN stack_profile_mapping mp ON mp.id = f.mapping
      UNION ALL
      SELECT
        w.root_cid,
        p.id,
        w.depth + 1,
        (${IS_ALLOCATOR_FRAME_SQL})
      FROM walk w
      JOIN stack_profile_callsite sc ON sc.id = w.cid
      JOIN stack_profile_callsite p ON p.id = sc.parent_id
      JOIN stack_profile_frame f ON f.id = p.frame_id
      LEFT JOIN stack_profile_mapping mp ON mp.id = f.mapping
      WHERE w.is_alloc = 1 AND w.depth < 12
    ),
    resolved_site AS (
      SELECT
        root_cid AS callsite_id,
        coalesce(MIN(CASE WHEN is_alloc = 0 THEN depth END), 0) AS chosen_depth
      FROM walk
      GROUP BY root_cid
    ),
    attributed_site AS (
      SELECT
        r.callsite_id,
        coalesce(f.deobfuscated_name, f.name, '<unknown>') AS attr_frame,
        coalesce(mp.name, '<unknown>') AS attr_mapping
      FROM resolved_site r
      JOIN walk w ON w.root_cid = r.callsite_id AND w.depth = r.chosen_depth
      JOIN stack_profile_callsite sc ON sc.id = w.cid
      JOIN stack_profile_frame f ON f.id = sc.frame_id
      LEFT JOIN stack_profile_mapping mp ON mp.id = f.mapping
    )
  `;
}

export interface NativeHeapSummary {
  readonly heapName: string;
  readonly dumps: number;
  readonly unreleasedBytes: number;
  readonly totalAllocBytes: number;
  readonly freedBytes: number;
  readonly unreleasedCount: number;
  readonly totalAllocCount: number;
}

export interface NativeProfileMeta {
  readonly heaps: ReadonlyArray<NativeHeapSummary>;
  readonly dumpTimestamps: ReadonlyArray<time>;
  readonly nativeSmapsRss: number;
  readonly threadStackBytes: number;
  readonly threadCount: number;
}

export interface NativeWindow {
  readonly heapName: string; // ALL_HEAPS or concrete heap_name
  readonly fromTs?: time; // exclusive lower bound for diff mode
  readonly toTs?: time; // inclusive upper bound
}

export interface NativeWindowTotals {
  readonly unreleasedBytes: number;
  readonly totalAllocBytes: number;
  readonly freedBytes: number;
  readonly freedPct: number;
  readonly unreleasedCount: number;
  readonly totalAllocCount: number;
}

export interface NativeLibraryRow {
  readonly mapping: string;
  readonly shortName: string;
  readonly isApp: boolean;
  readonly unreleasedBytes: number;
  readonly totalAllocBytes: number;
  readonly freedPct: number;
  readonly unreleasedCount: number;
  readonly totalAllocCount: number;
  readonly avgAllocBytes: number;
  readonly callsiteCount: number;
  readonly trend: ReadonlyArray<number>;
}

export interface NativeCallsiteRow {
  readonly callsiteId: number;
  readonly leafFrame: string;
  readonly mapping: string;
  readonly shortMapping: string;
  readonly isApp: boolean;
  readonly unreleasedBytes: number;
  readonly totalAllocBytes: number;
  readonly freedPct: number;
  readonly unreleasedCount: number;
  readonly totalAllocCount: number;
  readonly avgAllocBytes: number;
  readonly trend: ReadonlyArray<number>;
}

export interface NativeStackFrame {
  readonly depth: number;
  readonly name: string;
  readonly mapping: string;
  readonly shortMapping: string;
  readonly isApp: boolean;
}

export interface NativeIntervalPoint {
  readonly ts: time;
  readonly relS: number;
  readonly allocBytes: number;
  readonly freeBytes: number;
  readonly netBytes: number;
  readonly cumUnreleasedBytes: number;
}

export interface NativeLibrarySeries {
  readonly mapping: string;
  readonly shortName: string;
  readonly points: ReadonlyArray<{readonly x: number; readonly y: number}>;
  readonly firstBytes: number;
  readonly lastBytes: number;
  readonly deltaBytes: number;
  readonly ratePerHour: number | undefined;
}

export interface NativeGrowthData {
  readonly intervals: ReadonlyArray<NativeIntervalPoint>;
  readonly topLibraries: ReadonlyArray<NativeLibrarySeries>;
  readonly growingCallsites: ReadonlyArray<{
    readonly callsiteId: number;
    readonly leafFrame: string;
    readonly shortMapping: string;
    readonly firstBytes: number;
    readonly lastBytes: number;
    readonly deltaBytes: number;
    readonly ratePerHour: number | undefined;
    readonly totalAllocBytes: number;
  }>;
}

function escapeSqlStr(s: string): string {
  return s.replace(/'/g, "''");
}

function isAppMappingPath(mapping: string | undefined): boolean {
  if (!mapping) return false;
  return /^\/data\/(app|data)\//.test(mapping);
}

function shortMappingName(mapping: string): string {
  if (!mapping || mapping === '<unknown>') return '<unknown>';
  const parts = mapping.split('/');
  return parts[parts.length - 1] || mapping;
}

export function heapWhereClause(upid: number, win: NativeWindow): string {
  const parts = [`a.upid = ${upid}`];
  if (win.heapName !== ALL_HEAPS) {
    parts.push(`a.heap_name = '${escapeSqlStr(win.heapName)}'`);
  }
  if (win.fromTs !== undefined) {
    parts.push(`a.ts > ${win.fromTs}`);
  }
  if (win.toTs !== undefined) {
    parts.push(`a.ts <= ${win.toTs}`);
  }
  return parts.join(' AND ');
}

function computeFreedPct(
  unreleasedBytes: number,
  totalAllocBytes: number,
): number {
  if (totalAllocBytes <= 0) return 0;
  const freed = Math.max(0, totalAllocBytes - Math.max(0, unreleasedBytes));
  return Math.max(0, Math.min(100, (freed / totalAllocBytes) * 100));
}

export async function loadNativeProfileMeta(
  trace: Trace,
  upid: number,
): Promise<NativeProfileMeta> {
  const [heapsRes, tsRes, smapsTsRes, threadsRes] = await Promise.all([
    trace.engine.query(`
      WITH per_site AS (
        SELECT
          coalesce(heap_name, 'malloc') AS heap_name,
          callsite_id,
          SUM(size) AS net_size,
          SUM(count) AS net_count,
          SUM(CASE WHEN size > 0 THEN size ELSE 0 END) AS alloc_size,
          SUM(CASE WHEN size < 0 THEN -size ELSE 0 END) AS free_size,
          SUM(CASE WHEN count > 0 THEN count ELSE 0 END) AS alloc_count
        FROM heap_profile_allocation
        WHERE upid = ${upid}
        GROUP BY heap_name, callsite_id
      ),
      dump_cnt AS (
        SELECT coalesce(heap_name, 'malloc') AS heap_name, COUNT(DISTINCT ts) AS dumps
        FROM heap_profile_allocation
        WHERE upid = ${upid}
        GROUP BY heap_name
      )
      SELECT
        p.heap_name,
        d.dumps,
        CAST(SUM(CASE WHEN p.net_size > 0 THEN p.net_size ELSE 0 END) AS INT) AS unrel_bytes,
        CAST(SUM(p.alloc_size) AS INT) AS alloc_bytes,
        CAST(SUM(p.free_size) AS INT) AS free_bytes,
        CAST(SUM(CASE WHEN p.net_count > 0 THEN p.net_count ELSE 0 END) AS INT) AS unrel_count,
        CAST(SUM(p.alloc_count) AS INT) AS alloc_count
      FROM per_site p
      JOIN dump_cnt d USING (heap_name)
      GROUP BY p.heap_name
      ORDER BY unrel_bytes DESC, alloc_bytes DESC
    `),
    trace.engine.query(`
      SELECT DISTINCT ts
      FROM heap_profile_allocation
      WHERE upid = ${upid}
      ORDER BY ts ASC
    `),
    trace.engine.query(
      `SELECT MAX(ts) AS ts FROM profiler_smaps WHERE upid = ${upid}`,
    ),
    trace.engine.query(`SELECT COUNT(*) AS n FROM thread WHERE upid = ${upid}`),
  ]);

  const heaps: NativeHeapSummary[] = [];
  for (
    const it = heapsRes.iter({
      heap_name: STR,
      dumps: NUM,
      unrel_bytes: NUM,
      alloc_bytes: NUM,
      free_bytes: NUM,
      unrel_count: NUM,
      alloc_count: NUM,
    });
    it.valid();
    it.next()
  ) {
    heaps.push({
      heapName: it.heap_name,
      dumps: it.dumps,
      unreleasedBytes: it.unrel_bytes,
      totalAllocBytes: it.alloc_bytes,
      freedBytes: it.free_bytes,
      unreleasedCount: it.unrel_count,
      totalAllocCount: it.alloc_count,
    });
  }

  const dumpTimestamps: time[] = [];
  for (const it = tsRes.iter({ts: LONG}); it.valid(); it.next()) {
    dumpTimestamps.push(Time.fromRaw(it.ts));
  }

  let nativeSmapsRss = 0;
  let threadStackBytes = 0;
  const lastSmapsTs = smapsTsRes.firstRow({ts: LONG_NULL}).ts;
  if (lastSmapsTs !== null) {
    const catRes = await trace.engine.query(`
      INCLUDE PERFETTO MODULE memscope.smaps;
      SELECT category, anon_swap_bytes AS bytes
      FROM memscope_smaps_category_breakdown
      WHERE upid = ${upid} AND ts = ${lastSmapsTs}
    `);
    for (
      const it = catRes.iter({category: STR, bytes: NUM});
      it.valid();
      it.next()
    ) {
      if (it.category === 'native') nativeSmapsRss = it.bytes;
      else if (it.category === 'stack') threadStackBytes = it.bytes;
    }
  }

  return {
    heaps,
    dumpTimestamps,
    nativeSmapsRss,
    threadStackBytes,
    threadCount: threadsRes.firstRow({n: NUM}).n,
  };
}

export async function loadNativeWindowTotals(
  trace: Trace,
  upid: number,
  win: NativeWindow,
): Promise<NativeWindowTotals> {
  const where = heapWhereClause(upid, win);
  const res = await trace.engine.query(`
    WITH per_site AS (
      SELECT
        callsite_id,
        SUM(size) AS net_size,
        SUM(count) AS net_count,
        SUM(CASE WHEN size > 0 THEN size ELSE 0 END) AS alloc_size,
        SUM(CASE WHEN size < 0 THEN -size ELSE 0 END) AS free_size,
        SUM(CASE WHEN count > 0 THEN count ELSE 0 END) AS alloc_count
      FROM heap_profile_allocation a
      WHERE ${where}
      GROUP BY callsite_id
    )
    SELECT
      CAST(ifnull(SUM(CASE WHEN net_size > 0 THEN net_size ELSE 0 END), 0) AS INT) AS unrel_bytes,
      CAST(ifnull(SUM(alloc_size), 0) AS INT) AS alloc_bytes,
      CAST(ifnull(SUM(free_size), 0) AS INT) AS free_bytes,
      CAST(ifnull(SUM(CASE WHEN net_count > 0 THEN net_count ELSE 0 END), 0) AS INT) AS unrel_count,
      CAST(ifnull(SUM(alloc_count), 0) AS INT) AS alloc_count
    FROM per_site
  `);
  const r = res.firstRow({
    unrel_bytes: NUM,
    alloc_bytes: NUM,
    free_bytes: NUM,
    unrel_count: NUM,
    alloc_count: NUM,
  });
  return {
    unreleasedBytes: r.unrel_bytes,
    totalAllocBytes: r.alloc_bytes,
    freedBytes: r.free_bytes,
    freedPct: computeFreedPct(r.unrel_bytes, r.alloc_bytes),
    unreleasedCount: r.unrel_count,
    totalAllocCount: r.alloc_count,
  };
}

export async function loadNativeLibraries(
  trace: Trace,
  upid: number,
  win: NativeWindow,
  limit = 100,
): Promise<NativeLibraryRow[]> {
  const where = heapWhereClause(upid, win);
  const res = await trace.engine.query(`
    WITH RECURSIVE
    ${attributedSitesCte(where)},
    per_site AS (
      SELECT
        a.callsite_id,
        att.attr_mapping AS mapping,
        SUM(a.size) AS net_size,
        SUM(a.count) AS net_count,
        SUM(CASE WHEN a.size > 0 THEN a.size ELSE 0 END) AS alloc_size,
        SUM(CASE WHEN a.count > 0 THEN a.count ELSE 0 END) AS alloc_count
      FROM heap_profile_allocation a
      JOIN attributed_site att ON att.callsite_id = a.callsite_id
      WHERE ${where}
      GROUP BY a.callsite_id
    )
    SELECT
      mapping,
      CAST(SUM(CASE WHEN net_size > 0 THEN net_size ELSE 0 END) AS INT) AS unrel_bytes,
      CAST(SUM(alloc_size) AS INT) AS alloc_bytes,
      CAST(SUM(CASE WHEN net_count > 0 THEN net_count ELSE 0 END) AS INT) AS unrel_count,
      CAST(SUM(alloc_count) AS INT) AS alloc_count,
      COUNT(*) AS callsite_count
    FROM per_site
    GROUP BY mapping
    ORDER BY unrel_bytes DESC, alloc_bytes DESC
    LIMIT ${limit}
  `);

  const rows: NativeLibraryRow[] = [];
  for (
    const it = res.iter({
      mapping: STR,
      unrel_bytes: NUM,
      alloc_bytes: NUM,
      unrel_count: NUM,
      alloc_count: NUM,
      callsite_count: NUM,
    });
    it.valid();
    it.next()
  ) {
    rows.push({
      mapping: it.mapping,
      shortName: shortMappingName(it.mapping),
      isApp: isAppMappingPath(it.mapping),
      unreleasedBytes: it.unrel_bytes,
      totalAllocBytes: it.alloc_bytes,
      freedPct: computeFreedPct(it.unrel_bytes, it.alloc_bytes),
      unreleasedCount: it.unrel_count,
      totalAllocCount: it.alloc_count,
      avgAllocBytes:
        it.alloc_count > 0 ? Math.round(it.alloc_bytes / it.alloc_count) : 0,
      callsiteCount: it.callsite_count,
      trend: [],
    });
  }

  // Attach cumulative unreleased trend sparkline across timestamps if >= 2 dumps.
  const trendMap = await loadLibraryTrends(trace, upid, win);
  if (trendMap.size === 0) return rows;
  return rows.map((r) => ({...r, trend: trendMap.get(r.mapping) ?? []}));
}

async function loadLibraryTrends(
  trace: Trace,
  upid: number,
  win: NativeWindow,
): Promise<Map<string, number[]>> {
  const where = heapWhereClause(upid, win);
  const tsRes = await trace.engine.query(
    `SELECT DISTINCT ts FROM heap_profile_allocation a WHERE ${where} ORDER BY ts ASC`,
  );
  const timestamps: bigint[] = [];
  for (const it = tsRes.iter({ts: LONG}); it.valid(); it.next()) {
    timestamps.push(it.ts);
  }
  const out = new Map<string, number[]>();
  if (timestamps.length < 2) return out;

  const res = await trace.engine.query(`
    WITH RECURSIVE
    ${attributedSitesCte(where)}
    SELECT
      att.attr_mapping AS mapping,
      a.ts,
      CAST(SUM(a.size) AS INT) AS delta_bytes
    FROM heap_profile_allocation a
    JOIN attributed_site att ON att.callsite_id = a.callsite_id
    WHERE ${where}
    GROUP BY mapping, a.ts
    ORDER BY mapping, a.ts ASC
  `);
  const byLibTs = new Map<string, Map<bigint, number>>();
  for (
    const it = res.iter({mapping: STR, ts: LONG, delta_bytes: NUM});
    it.valid();
    it.next()
  ) {
    let m = byLibTs.get(it.mapping);
    if (m === undefined) {
      m = new Map();
      byLibTs.set(it.mapping, m);
    }
    m.set(it.ts, it.delta_bytes);
  }
  for (const [lib, tsMap] of byLibTs) {
    let cum = 0;
    const pts: number[] = [];
    for (const ts of timestamps) {
      cum += tsMap.get(ts) ?? 0;
      pts.push(Math.max(0, cum));
    }
    out.set(lib, pts);
  }
  return out;
}

export async function loadNativeCallsites(
  trace: Trace,
  upid: number,
  win: NativeWindow,
  opts: {
    readonly sortBy?: 'unreleased' | 'alloc';
    readonly mappingFilter?: string;
    readonly limit?: number;
  } = {},
): Promise<NativeCallsiteRow[]> {
  const {sortBy = 'unreleased', mappingFilter, limit = 120} = opts;
  const where = heapWhereClause(upid, win);
  const mappingClause =
    mappingFilter !== undefined
      ? ` AND att.attr_mapping = '${escapeSqlStr(mappingFilter)}'`
      : '';
  const orderCol =
    sortBy === 'alloc'
      ? 'alloc_bytes DESC, unrel_bytes DESC'
      : 'unrel_bytes DESC, alloc_bytes DESC';

  const res = await trace.engine.query(`
    WITH RECURSIVE
    ${attributedSitesCte(where)}
    SELECT
      a.callsite_id,
      att.attr_frame AS leaf_frame,
      att.attr_mapping AS mapping,
      CAST(MAX(SUM(a.size), 0) AS INT) AS unrel_bytes,
      CAST(SUM(CASE WHEN a.size > 0 THEN a.size ELSE 0 END) AS INT) AS alloc_bytes,
      CAST(MAX(SUM(a.count), 0) AS INT) AS unrel_count,
      CAST(SUM(CASE WHEN a.count > 0 THEN a.count ELSE 0 END) AS INT) AS alloc_count
    FROM heap_profile_allocation a
    JOIN attributed_site att ON att.callsite_id = a.callsite_id
    WHERE ${where}${mappingClause}
    GROUP BY a.callsite_id
    HAVING unrel_bytes > 0 OR alloc_bytes > 0
    ORDER BY ${orderCol}
    LIMIT ${limit}
  `);

  const rows: NativeCallsiteRow[] = [];
  for (
    const it = res.iter({
      callsite_id: NUM,
      leaf_frame: STR,
      mapping: STR,
      unrel_bytes: NUM,
      alloc_bytes: NUM,
      unrel_count: NUM,
      alloc_count: NUM,
    });
    it.valid();
    it.next()
  ) {
    rows.push({
      callsiteId: it.callsite_id,
      leafFrame: it.leaf_frame,
      mapping: it.mapping,
      shortMapping: shortMappingName(it.mapping),
      isApp: isAppMappingPath(it.mapping),
      unreleasedBytes: it.unrel_bytes,
      totalAllocBytes: it.alloc_bytes,
      freedPct: computeFreedPct(it.unrel_bytes, it.alloc_bytes),
      unreleasedCount: it.unrel_count,
      totalAllocCount: it.alloc_count,
      avgAllocBytes:
        it.alloc_count > 0 ? Math.round(it.alloc_bytes / it.alloc_count) : 0,
      trend: [],
    });
  }
  return rows;
}

export async function loadCallsiteStack(
  trace: Trace,
  callsiteId: number,
): Promise<NativeStackFrame[]> {
  const res = await trace.engine.query(`
    WITH RECURSIVE chain(cid, depth) AS (
      SELECT ${callsiteId}, 0
      UNION ALL
      SELECT sc.parent_id, c.depth + 1
      FROM chain c
      JOIN stack_profile_callsite sc ON sc.id = c.cid
      WHERE sc.parent_id IS NOT NULL AND c.depth < 96
    )
    SELECT
      c.depth,
      coalesce(f.deobfuscated_name, f.name, '<unknown>') AS frame_name,
      coalesce(mp.name, '<unknown>') AS mapping_name
    FROM chain c
    JOIN stack_profile_callsite sc ON sc.id = c.cid
    JOIN stack_profile_frame f ON f.id = sc.frame_id
    LEFT JOIN stack_profile_mapping mp ON mp.id = f.mapping
    ORDER BY c.depth ASC
  `);
  const frames: NativeStackFrame[] = [];
  for (
    const it = res.iter({
      depth: NUM,
      frame_name: STR,
      mapping_name: STR_NULL,
    });
    it.valid();
    it.next()
  ) {
    const mapping = it.mapping_name ?? '<unknown>';
    frames.push({
      depth: it.depth,
      name: it.frame_name,
      mapping,
      shortMapping: shortMappingName(mapping),
      isApp: isAppMappingPath(mapping),
    });
  }
  return frames;
}

export async function loadNativeGrowthData(
  trace: Trace,
  upid: number,
  heapName: string,
): Promise<NativeGrowthData> {
  const where = heapWhereClause(upid, {heapName});
  const t0 = trace.traceInfo.start;

  const intervalRes = await trace.engine.query(`
    SELECT
      a.ts,
      CAST(SUM(CASE WHEN a.size > 0 THEN a.size ELSE 0 END) AS INT) AS alloc_bytes,
      CAST(SUM(CASE WHEN a.size < 0 THEN -a.size ELSE 0 END) AS INT) AS free_bytes,
      CAST(SUM(a.size) AS INT) AS net_bytes
    FROM heap_profile_allocation a
    WHERE ${where}
    GROUP BY a.ts
    ORDER BY a.ts ASC
  `);

  const intervals: NativeIntervalPoint[] = [];
  let cum = 0;
  for (
    const it = intervalRes.iter({
      ts: LONG,
      alloc_bytes: NUM,
      free_bytes: NUM,
      net_bytes: NUM,
    });
    it.valid();
    it.next()
  ) {
    cum += it.net_bytes;
    intervals.push({
      ts: Time.fromRaw(it.ts),
      relS: Number(it.ts - t0) / 1e9,
      allocBytes: it.alloc_bytes,
      freeBytes: it.free_bytes,
      netBytes: it.net_bytes,
      cumUnreleasedBytes: Math.max(0, cum),
    });
  }

  if (intervals.length < 2) {
    return {intervals, topLibraries: [], growingCallsites: []};
  }

  const firstTs = intervals[0]!.ts;
  const lastTs = intervals[intervals.length - 1]!.ts;
  const spanS = Number(lastTs - firstTs) / 1e9;

  // Per-library cumulative unreleased time-series for top 6 libraries.
  const libRes = await trace.engine.query(`
    WITH RECURSIVE
    ${attributedSitesCte(where)}
    SELECT
      att.attr_mapping AS mapping,
      a.ts,
      CAST(SUM(a.size) AS INT) AS delta_bytes
    FROM heap_profile_allocation a
    JOIN attributed_site att ON att.callsite_id = a.callsite_id
    WHERE ${where}
    GROUP BY mapping, a.ts
    ORDER BY mapping, a.ts ASC
  `);
  const byLibTs = new Map<string, Map<bigint, number>>();
  for (
    const it = libRes.iter({mapping: STR, ts: LONG, delta_bytes: NUM});
    it.valid();
    it.next()
  ) {
    let m = byLibTs.get(it.mapping);
    if (m === undefined) {
      m = new Map();
      byLibTs.set(it.mapping, m);
    }
    m.set(it.ts, it.delta_bytes);
  }

  const topLibraries: NativeLibrarySeries[] = [];
  for (const [mapping, tsMap] of byLibTs) {
    let running = 0;
    const points: {x: number; y: number}[] = [];
    for (const iv of intervals) {
      running += tsMap.get(iv.ts) ?? 0;
      points.push({x: iv.relS, y: Math.max(0, running)});
    }
    const firstBytes = points[0]?.y ?? 0;
    const lastBytes = points[points.length - 1]?.y ?? 0;
    const deltaBytes = lastBytes - firstBytes;
    if (lastBytes > 0 || Math.abs(deltaBytes) > 0) {
      topLibraries.push({
        mapping,
        shortName: shortMappingName(mapping),
        points,
        firstBytes,
        lastBytes,
        deltaBytes,
        ratePerHour: spanS > 0 ? (deltaBytes / spanS) * 3600 : undefined,
      });
    }
  }
  topLibraries.sort(
    (a, b) =>
      Math.abs(b.deltaBytes) - Math.abs(a.deltaBytes) ||
      b.lastBytes - a.lastBytes,
  );

  // Fastest growing callsites between first dump and last dump.
  const csRes = await trace.engine.query(`
    WITH RECURSIVE
    ${attributedSitesCte(where)}
    SELECT
      a.callsite_id,
      att.attr_frame AS leaf_frame,
      att.attr_mapping AS mapping,
      CAST(SUM(CASE WHEN a.ts = ${firstTs} THEN a.size ELSE 0 END) AS INT) AS first_bytes,
      CAST(SUM(a.size) AS INT) AS last_bytes,
      CAST(SUM(CASE WHEN a.ts > ${firstTs} THEN a.size ELSE 0 END) AS INT) AS delta_bytes,
      CAST(SUM(CASE WHEN a.size > 0 THEN a.size ELSE 0 END) AS INT) AS alloc_bytes
    FROM heap_profile_allocation a
    JOIN attributed_site att ON att.callsite_id = a.callsite_id
    WHERE ${where}
    GROUP BY a.callsite_id
    HAVING delta_bytes > 0
    ORDER BY delta_bytes DESC
    LIMIT 30
  `);
  const growingCallsites: NativeGrowthData['growingCallsites'][number][] = [];
  for (
    const it = csRes.iter({
      callsite_id: NUM,
      leaf_frame: STR,
      mapping: STR,
      first_bytes: NUM,
      last_bytes: NUM,
      delta_bytes: NUM,
      alloc_bytes: NUM,
    });
    it.valid();
    it.next()
  ) {
    growingCallsites.push({
      callsiteId: it.callsite_id,
      leafFrame: it.leaf_frame,
      shortMapping: shortMappingName(it.mapping),
      firstBytes: Math.max(0, it.first_bytes),
      lastBytes: Math.max(0, it.last_bytes),
      deltaBytes: it.delta_bytes,
      ratePerHour: spanS > 0 ? (it.delta_bytes / spanS) * 3600 : undefined,
      totalAllocBytes: it.alloc_bytes,
    });
  }

  return {
    intervals,
    topLibraries: topLibraries.slice(0, 6),
    growingCallsites,
  };
}

export interface NativeSunburstLib {
  readonly mapping: string;
  readonly shortName: string;
  readonly bytes: number;
}

export interface NativeSunburstFunc {
  readonly mapping: string;
  readonly shortMapping: string;
  readonly frame: string;
  readonly bytes: number;
}

// Lightweight top-libraries breakdown by unreleased native bytes for the
// Overview sunburst when smaps is absent and heapprofd is present.
export async function loadTopNativeLibsForSunburst(
  trace: Trace,
  upid: number,
  limit = 6,
): Promise<ReadonlyArray<NativeSunburstLib>> {
  const where = `a.upid = ${upid}`;
  const res = await trace.engine.query(`
    WITH RECURSIVE
    ${attributedSitesCte(where)},
    per_site AS (
      SELECT
        a.callsite_id,
        att.attr_mapping AS mapping,
        SUM(a.size) AS net_bytes
      FROM heap_profile_allocation a
      JOIN attributed_site att ON att.callsite_id = a.callsite_id
      WHERE ${where}
      GROUP BY a.callsite_id
      HAVING net_bytes > 0
    )
    SELECT
      mapping,
      CAST(SUM(net_bytes) AS INT) AS bytes
    FROM per_site
    GROUP BY mapping
    ORDER BY bytes DESC
    LIMIT ${limit + 1}
  `);
  const rows: NativeSunburstLib[] = [];
  for (const it = res.iter({mapping: STR, bytes: NUM}); it.valid(); it.next()) {
    if (it.bytes <= 0) continue;
    rows.push({
      mapping: it.mapping,
      shortName: shortMappingName(it.mapping),
      bytes: it.bytes,
    });
  }
  return rows;
}

// Library -> attributed function breakdown for heapprofd-only Overview sunburst.
export async function loadNativeLibFunctionsForSunburst(
  trace: Trace,
  upid: number,
  limit = 40,
): Promise<ReadonlyArray<NativeSunburstFunc>> {
  const where = `a.upid = ${upid}`;
  const res = await trace.engine.query(`
    WITH RECURSIVE
    ${attributedSitesCte(where)},
    per_site AS (
      SELECT
        a.callsite_id,
        att.attr_mapping AS mapping,
        att.attr_frame AS frame,
        SUM(a.size) AS net_bytes
      FROM heap_profile_allocation a
      JOIN attributed_site att ON att.callsite_id = a.callsite_id
      WHERE ${where}
      GROUP BY a.callsite_id
      HAVING net_bytes > 0
    )
    SELECT
      mapping,
      frame,
      CAST(SUM(net_bytes) AS INT) AS bytes
    FROM per_site
    GROUP BY mapping, frame
    ORDER BY bytes DESC
    LIMIT ${limit}
  `);
  const rows: NativeSunburstFunc[] = [];
  for (
    const it = res.iter({mapping: STR, frame: STR, bytes: NUM});
    it.valid();
    it.next()
  ) {
    if (it.bytes <= 0) continue;
    rows.push({
      mapping: it.mapping,
      shortMapping: shortMappingName(it.mapping),
      frame: it.frame,
      bytes: it.bytes,
    });
  }
  return rows;
}
