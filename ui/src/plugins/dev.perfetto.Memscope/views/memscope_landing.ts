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

import './memscope_landing.scss';
import m from 'mithril';
import {QuerySlot, type QueryResult} from '../../../base/query_slot';
import {Duration, Time, type time} from '../../../base/time';
import {Timestamp} from '../../../components/widgets/timestamp';
import type {Trace} from '../../../public/trace';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../../trace_processor/query_result';
import {Button} from '../../../widgets/button';

import {Chip} from '../../../widgets/chip';
import {Icon} from '../../../widgets/icon';
import {Intent} from '../../../widgets/common';
import {
  LineChartSvg,
  type LineChartData,
} from '../../../components/widgets/charts_svg/line_chart_svg';
import {Panel} from '../components/panel';
import {Select} from '../../../widgets/select';

interface HeapDumpRow {
  ts: time;
  upid: number;
  pid: number;
  processName: string;
  eventId: number;
  eventType: string;
  totalSize: number;
  reachableSize: number;
  objectCount: number;
  jniGlobalSize: number;
  jniLocalSize: number;
}

interface HeapProfileRow {
  ts: time;
  upid: number;
  pid: number;
  processName: string;
  heapName: string;
  samples: number;
  totalSize: number;
  releasedSize: number;
  unreleasedSize: number;
  eventId: number;
  eventType: string;
}

interface JavaHeapCounterRow {
  ts: time;
  upid: number;
  pid: number;
  processName: string;
  heapSize: number;
}

interface SmapsSummaryRow {
  ts: time;
  upid: number;
  pid: number;
  processName: string;
  rss: number;
  pss: number;
  anonAndSwap: number;
  dalvikAnonAndSwap: number;
  nativeAnonAndSwap: number;
  otherAnonAndSwap: number;
  trackId?: number;
}

// Per-class retained bytes within a single Java heap dump, split by whether
// the objects are reachable from a GC root.
interface HeapDumpClassRow {
  ts: time;
  upid: number;
  processName: string;
  className: string;
  reachable: boolean;
  size: number;
}

interface Data {
  heapDumps: HeapDumpRow[];
  heapDumpClasses: HeapDumpClassRow[];
  heapProfiles: HeapProfileRow[];
  javaHeapCounters: JavaHeapCounterRow[];
  smapsSummaries: SmapsSummaryRow[];
}

type SnapshotEntry =
  | {kind: 'dump'; row: HeapDumpRow}
  | {kind: 'profile'; row: HeapProfileRow}
  | {kind: 'smaps'; row: SmapsSummaryRow};

// Everything derived from (raw data, selected process) that doesn't depend on
// per-frame state. Computed once per selection in computeViewModel() and reused
// across redraws — the data is immutable after load, so none of this changes
// until the user picks a different process.
interface ViewModel {
  filteredData: Data;
  hdChartData?: LineChartData;
  npChartData?: LineChartData;
  smapsChartData?: LineChartData;
  snapshots: SnapshotEntry[];
  // Δ in native unreleased vs the previous heapprofd sample, keyed by eventId.
  nativeDeltaByEventId: Map<number, number>;
  // Reachable Java heap bytes per dump, keyed by `upid-ts`.
  reachableByDump: Map<string, number>;
  // Δ in reachable bytes vs the previous dump, keyed by `upid-ts`.
  javaReachableDeltaByKey: Map<string, number>;
}

async function loadData(trace: Trace): Promise<Data> {
  const heapDumps: HeapDumpRow[] = [];
  const dumpRes = await trace.engine.query(`
    SELECT
      e.id AS event_id,
      e.type AS event_type,
      e.ts AS ts,
      e.upid AS upid,
      p.pid AS pid,
      coalesce(p.cmdline, p.name, '<unknown>') AS pname,
      CAST(ifnull(SUM(o.self_size + o.native_size), 0) AS INT) AS total_size,
      COUNT(o.id) AS object_count
    FROM heap_profile_events e
    JOIN process p USING (upid)
    LEFT JOIN heap_graph_object o
      ON o.upid = e.upid AND o.graph_sample_ts = e.ts
    WHERE e.type = 'java_heap_graph'
    GROUP BY e.id
    ORDER BY e.ts ASC
  `);
  for (
    const it = dumpRes.iter({
      event_id: NUM,
      event_type: STR,
      ts: LONG,
      upid: NUM,
      pid: NUM_NULL,
      pname: STR,
      total_size: NUM,
      object_count: NUM,
    });
    it.valid();
    it.next()
  ) {
    heapDumps.push({
      ts: Time.fromRaw(it.ts),
      upid: it.upid,
      pid: it.pid ?? 0,
      processName: it.pname,
      eventId: it.event_id,
      eventType: it.event_type,
      totalSize: it.total_size,
      reachableSize: 0,
      objectCount: it.object_count,
      jniGlobalSize: 0,
      jniLocalSize: 0,
    });
  }

  // Per-class retained bytes within each Java heap dump. Single scan of
  // heap_graph_object joined to its class, grouped by (dump, class) — same
  // pass over the heap as the totals query above, just a finer GROUP BY.
  const heapDumpClasses: HeapDumpClassRow[] = [];
  const classRes = await trace.engine.query(`
    SELECT
      o.graph_sample_ts AS ts,
      o.upid AS upid,
      coalesce(p.cmdline, p.name, '<unknown>') AS pname,
      coalesce(c.deobfuscated_name, c.name, '<unknown>') AS class_name,
      o.reachable AS reachable,
      CAST(SUM(o.self_size + o.native_size) AS INT) AS size
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    JOIN process p USING (upid)
    GROUP BY o.graph_sample_ts, o.upid, class_name, o.reachable
    ORDER BY o.graph_sample_ts ASC
  `);
  for (
    const it = classRes.iter({
      ts: LONG,
      upid: NUM,
      pname: STR,
      class_name: STR,
      reachable: NUM,
      size: NUM,
    });
    it.valid();
    it.next()
  ) {
    heapDumpClasses.push({
      ts: Time.fromRaw(it.ts),
      upid: it.upid,
      processName: it.pname,
      className: it.class_name,
      reachable: it.reachable !== 0,
      size: it.size,
    });
  }

  const heapProfiles: HeapProfileRow[] = [];
  const profRes = await trace.engine.query(`
    WITH snapshot_deltas AS (
      SELECT
        MIN(a.id) AS event_id,
        a.ts AS ts,
        a.upid AS upid,
        a.heap_name AS heap_name,
        COUNT(*) AS samples,
        SUM(CASE WHEN a.size > 0 THEN a.size ELSE 0 END) AS allocated_delta,
        SUM(a.size) AS unreleased_delta
      FROM heap_profile_allocation a
      GROUP BY a.ts, a.upid, a.heap_name
    ),
    running_sums AS (
      SELECT
        d.event_id,
        d.ts,
        d.upid,
        d.heap_name,
        d.samples,
        SUM(d.allocated_delta) OVER (
          PARTITION BY d.upid, d.heap_name
          ORDER BY d.ts
          ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
        ) AS total_size_running,
        SUM(d.unreleased_delta) OVER (
          PARTITION BY d.upid, d.heap_name
          ORDER BY d.ts
          ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
        ) AS unreleased_size_running
      FROM snapshot_deltas d
    )
    SELECT
      r.event_id,
      'heap_profile:' || r.heap_name AS event_type,
      r.ts AS ts,
      r.upid AS upid,
      p.pid AS pid,
      coalesce(p.cmdline, p.name, '<unknown>') AS pname,
      r.heap_name AS heap_name,
      r.samples AS samples,
      CAST(r.total_size_running AS INT) AS total_size,
      CAST(r.unreleased_size_running AS INT) AS unreleased_size
    FROM running_sums r
    JOIN process p USING (upid)
    ORDER BY r.ts ASC
  `);
  for (
    const it = profRes.iter({
      event_id: NUM,
      event_type: STR,
      ts: LONG,
      upid: NUM,
      pid: NUM_NULL,
      pname: STR,
      heap_name: STR_NULL,
      samples: NUM,
      total_size: NUM,
      unreleased_size: NUM,
    });
    it.valid();
    it.next()
  ) {
    heapProfiles.push({
      ts: Time.fromRaw(it.ts),
      upid: it.upid,
      pid: it.pid ?? 0,
      processName: it.pname,
      heapName: it.heap_name ?? 'malloc',
      samples: it.samples,
      totalSize: it.total_size,
      releasedSize: Math.max(0, it.total_size - it.unreleased_size),
      unreleasedSize: it.unreleased_size,
      eventId: it.event_id,
      eventType: it.event_type,
    });
  }

  const javaHeapCounters: JavaHeapCounterRow[] = [];
  const counterRes = await trace.engine.query(`
    SELECT
      c.ts AS ts,
      p.upid AS upid,
      p.pid AS pid,
      coalesce(p.cmdline, p.name, '<unknown>') AS pname,
      CAST(c.value * 1024 AS INT) AS heap_size
    FROM counter c
    JOIN process_counter_track t ON c.track_id = t.id
    JOIN process p ON t.upid = p.upid
    WHERE t.name = 'Heap size (KB)'
    ORDER BY c.ts ASC
  `);
  for (
    const it = counterRes.iter({
      ts: LONG,
      upid: NUM,
      pid: NUM_NULL,
      pname: STR,
      heap_size: NUM,
    });
    it.valid();
    it.next()
  ) {
    javaHeapCounters.push({
      ts: Time.fromRaw(it.ts),
      upid: it.upid,
      pid: it.pid ?? 0,
      processName: it.pname,
      heapSize: it.heap_size,
    });
  }

  const smapsSummaries: SmapsSummaryRow[] = [];
  const smapsRes = await trace.engine.query(`
    WITH grouped_smaps AS (
      SELECT
        s.ts AS ts,
        MIN(p.upid) AS upid,
        MIN(p.pid) AS pid,
        coalesce(p.name, '<unknown>') AS pname,
        CAST(ifnull(SUM(s.rss_kb), 0) * 1024 AS INT) AS rss,
        CAST(ifnull(SUM(s.proportional_resident_kb), 0) * 1024 AS INT) AS pss,
        CAST(ifnull(SUM(s.anonymous_kb + s.swap_kb), 0) * 1024 AS INT)
          AS anon_and_swap,
        -- Java heap: dalvik object/zygote/etc spaces.
        CAST(ifnull(SUM(CASE
          WHEN s.path GLOB '*dalvik*' OR s.path GLOB '/dev/ashmem/dalvik*'
          THEN s.anonymous_kb + s.swap_kb ELSE 0 END), 0) * 1024 AS INT)
          AS dalvik_anon_and_swap,
        -- Native heap: malloc allocator arenas (scudo / libc_malloc /
        -- jemalloc / GWP-ASan) and the legacy brk heap. A subset of native
        -- memory; stacks, graphics and mmap'd regions fall into "other".
        CAST(ifnull(SUM(CASE
          WHEN s.path GLOB '[anon:scudo*' OR s.path GLOB '[anon:libc_malloc*'
            OR s.path GLOB '[anon:jemalloc*' OR s.path GLOB '[anon:GWP-ASan*'
            OR s.path = '[heap]'
          THEN s.anonymous_kb + s.swap_kb ELSE 0 END), 0) * 1024 AS INT)
          AS native_anon_and_swap,
        -- Everything else: stacks, graphics, mmap, unnamed anon.
        CAST(ifnull(SUM(CASE
          WHEN s.path GLOB '*dalvik*' OR s.path GLOB '/dev/ashmem/dalvik*'
            OR s.path GLOB '[anon:scudo*' OR s.path GLOB '[anon:libc_malloc*'
            OR s.path GLOB '[anon:jemalloc*' OR s.path GLOB '[anon:GWP-ASan*'
            OR s.path = '[heap]'
          THEN 0 ELSE s.anonymous_kb + s.swap_kb END), 0) * 1024 AS INT)
          AS other_anon_and_swap
      FROM profiler_smaps s
      LEFT JOIN process p USING (upid)
      GROUP BY s.ts, p.name
    )
    SELECT
      g.ts AS ts,
      g.upid AS upid,
      g.pid AS pid,
      g.pname AS pname,
      g.rss AS rss,
      g.pss AS pss,
      g.anon_and_swap AS anon_and_swap,
      g.dalvik_anon_and_swap AS dalvik_anon_and_swap,
      g.native_anon_and_swap AS native_anon_and_swap,
      g.other_anon_and_swap AS other_anon_and_swap,
      (
        SELECT id FROM counter_track
        WHERE type = 'smaps' AND extract_arg(dimension_arg_set_id, 'upid') = g.upid
        LIMIT 1
      ) AS track_id
    FROM grouped_smaps g
    ORDER BY g.ts ASC
  `);
  for (
    const it = smapsRes.iter({
      ts: LONG,
      upid: NUM,
      pid: NUM_NULL,
      pname: STR,
      rss: NUM,
      pss: NUM,
      anon_and_swap: NUM,
      dalvik_anon_and_swap: NUM,
      native_anon_and_swap: NUM,
      other_anon_and_swap: NUM,
      track_id: NUM_NULL,
    });
    it.valid();
    it.next()
  ) {
    smapsSummaries.push({
      ts: Time.fromRaw(it.ts),
      upid: it.upid,
      pid: it.pid ?? 0,
      processName: it.pname,
      rss: it.rss,
      pss: it.pss,
      anonAndSwap: it.anon_and_swap,
      dalvikAnonAndSwap: it.dalvik_anon_and_swap,
      nativeAnonAndSwap: it.native_anon_and_swap,
      otherAnonAndSwap: it.other_anon_and_swap,
      trackId: it.track_id ?? undefined,
    });
  }

  return {
    heapDumps,
    heapDumpClasses,
    heapProfiles,
    javaHeapCounters,
    smapsSummaries,
  };
}

function trackUriFor(upid: number, type: string): string {
  return `/process_${upid}/${type}_heap_profile`;
}

function formatBytes(bytes: number): string {
  const sign = bytes < 0 ? '-' : '';
  const absBytes = Math.abs(bytes);
  if (absBytes < 1024) return `${sign}${absBytes} B`;
  const units = ['KiB', 'MiB', 'GiB'];
  let v = absBytes / 1024;
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return `${sign}${v.toFixed(2)} ${units[i]}`;
}

// Formats a signed byte delta, e.g. "+1.20 MiB" / "-512 B" / "±0 B".
function formatDelta(bytes: number): string {
  if (bytes === 0) return '±0 B';
  return (bytes > 0 ? '+' : '-') + formatBytes(Math.abs(bytes));
}

// Span in seconds between the first and last sample of a ts-sorted series.
// Uses the real sample timestamps — no fixed cadence is assumed.
function sampleSpanSeconds<T extends {ts: time}>(
  rows: T[],
): number | undefined {
  if (rows.length < 2) return undefined;
  return Number(rows[rows.length - 1].ts - rows[0].ts) / 1e9;
}

function getSortedSnapshots(data: Data): SnapshotEntry[] {
  const entries: SnapshotEntry[] = [
    ...data.heapDumps.map((row): SnapshotEntry => ({kind: 'dump', row})),
    ...data.heapProfiles.map((row): SnapshotEntry => ({kind: 'profile', row})),
    ...data.smapsSummaries.map((row): SnapshotEntry => ({kind: 'smaps', row})),
  ];
  entries.sort((a, b) => {
    if (a.row.ts < b.row.ts) return -1;
    if (a.row.ts > b.row.ts) return 1;
    return 0;
  });
  return entries;
}

// Number of distinct classes to show as their own band; the rest collapse
// into "Other".
const HEAP_DUMP_TOP_CLASSES = 5;

// Trims a fully-qualified class name to its last segment(s) for legends, e.g.
// 'java.util.HashMap$Node' -> 'HashMap$Node'. Arrays and 'Other' pass through.
function shortClassName(name: string): string {
  if (name === 'Other') return name;
  const arrayDepth = (name.match(/\[\]/g) ?? []).length;
  const base = name.replace(/\[\]/g, '');
  const last = base.slice(base.lastIndexOf('.') + 1);
  return last + '[]'.repeat(arrayDepth);
}

const HEAP_DUMP_OTHER_REACHABLE = 'Other (reachable)';
const HEAP_DUMP_UNREACHABLE = 'Unreachable';

// Builds the Java heap composition chart: a stacked area over time. Reachable
// objects are split into the top-N classes (by peak retained bytes) plus an
// "Other (reachable)" band; all unreachable objects collapse into a single
// "Unreachable" band drawn on top.
function buildHeapDumpsChartData(
  trace: Trace,
  heapDumpClasses: HeapDumpClassRow[],
): LineChartData | undefined {
  if (heapDumpClasses.length === 0) return undefined;

  // Distinct dump timestamps (the x-axis), ascending. Every series must have a
  // point at each of these for stacking to line up.
  const timestamps = Array.from(new Set(heapDumpClasses.map((r) => r.ts))).sort(
    (a, b) => (a < b ? -1 : a > b ? 1 : 0),
  );

  // Rank classes by their peak reachable bytes across all dumps — the top-N
  // are the bands that get their own series.
  const peakByClass = new Map<string, number>();
  for (const r of heapDumpClasses) {
    if (!r.reachable) continue;
    peakByClass.set(
      r.className,
      Math.max(peakByClass.get(r.className) ?? 0, r.size),
    );
  }
  const topClasses = new Set(
    Array.from(peakByClass.entries())
      .sort((a, b) => b[1] - a[1])
      .slice(0, HEAP_DUMP_TOP_CLASSES)
      .map(([name]) => name),
  );

  // size[seriesName][ts] — reachable bucketed into top-N / "Other (reachable)",
  // unreachable all into a single band.
  const sizeByLabel = new Map<string, Map<time, number>>();
  const bump = (label: string, ts: time, size: number) => {
    let byTs = sizeByLabel.get(label);
    if (!byTs) {
      byTs = new Map();
      sizeByLabel.set(label, byTs);
    }
    byTs.set(ts, (byTs.get(ts) ?? 0) + size);
  };
  for (const r of heapDumpClasses) {
    if (!r.reachable) {
      bump(HEAP_DUMP_UNREACHABLE, r.ts, r.size);
    } else {
      bump(
        topClasses.has(r.className) ? r.className : HEAP_DUMP_OTHER_REACHABLE,
        r.ts,
        r.size,
      );
    }
  }

  // Stack order, bottom→top: top reachable classes (largest first), then
  // "Other (reachable)", then "Unreachable" on top.
  const orderedLabels = [
    ...Array.from(topClasses).sort(
      (a, b) => (peakByClass.get(b) ?? 0) - (peakByClass.get(a) ?? 0),
    ),
    ...(sizeByLabel.has(HEAP_DUMP_OTHER_REACHABLE)
      ? [HEAP_DUMP_OTHER_REACHABLE]
      : []),
    ...(sizeByLabel.has(HEAP_DUMP_UNREACHABLE) ? [HEAP_DUMP_UNREACHABLE] : []),
  ];

  const series = orderedLabels.map((label) => {
    const byTs = sizeByLabel.get(label) ?? new Map();
    return {
      // Top-N entries are class names (trim); the two summary bands pass through.
      name:
        label === HEAP_DUMP_OTHER_REACHABLE || label === HEAP_DUMP_UNREACHABLE
          ? label
          : shortClassName(label),
      points: timestamps.map((ts) => ({
        x: Number(ts - trace.traceInfo.start) / 1e9,
        y: byTs.get(ts) ?? 0,
      })),
    };
  });
  return {series};
}

function buildHeapProfilesChartData(
  trace: Trace,
  heapProfiles: HeapProfileRow[],
): LineChartData | undefined {
  if (heapProfiles.length === 0) return undefined;
  // Stacked: Unreleased (live, bottom) + Released (freed, top) sum to the
  // total ever allocated. The band thicknesses read directly as leak (live)
  // vs churn (freed).
  const unreleasedSeriesMap = new Map<string, {x: number; y: number}[]>();
  const releasedSeriesMap = new Map<string, {x: number; y: number}[]>();

  const append = (
    map: Map<string, {x: number; y: number}[]>,
    key: string,
    x: number,
    y: number,
  ) => {
    let points = map.get(key);
    if (!points) {
      points = [];
      map.set(key, points);
    }
    points.push({x, y});
  };

  for (const r of heapProfiles) {
    const key = `${r.processName} - ${r.heapName}`;
    const x = Number(r.ts - trace.traceInfo.start) / 1e9;
    append(unreleasedSeriesMap, key, x, r.unreleasedSize);
    append(releasedSeriesMap, key, x, r.releasedSize);
  }

  const series = [];
  for (const [name, points] of releasedSeriesMap.entries()) {
    series.push({name: `${name} (Released)`, points});
  }
  for (const [name, points] of unreleasedSeriesMap.entries()) {
    series.push({name: `${name} (Unreleased)`, points});
  }
  return {series};
}

interface ProcessOption {
  name: string;
  heapDumps: number;
  heapProfiles: number;
  smaps: number;
}

function getUniqueProcesses(data: Data): ProcessOption[] {
  const counts = new Map<string, ProcessOption>();
  const get = (name: string): ProcessOption => {
    let opt = counts.get(name);
    if (!opt) {
      opt = {name, heapDumps: 0, heapProfiles: 0, smaps: 0};
      counts.set(name, opt);
    }
    return opt;
  };
  for (const r of data.heapDumps) {
    if (r.processName) get(r.processName).heapDumps++;
  }
  for (const r of data.heapProfiles) {
    if (r.processName) get(r.processName).heapProfiles++;
  }
  for (const r of data.javaHeapCounters) {
    if (r.processName) get(r.processName);
  }
  for (const r of data.smapsSummaries) {
    if (r.processName) get(r.processName).smaps++;
  }
  return Array.from(counts.values()).sort((a, b) =>
    a.name.localeCompare(b.name),
  );
}

// Scores a process to determine how relevant it is for the landing page.
// Higher score = more relevant. We weight by data type and count to pick
// the process with the richest memory analysis data.
function scoreProcess(p: ProcessOption): number {
  // Heap dumps are the richest data source, followed by smaps, then profiles.
  return p.heapDumps * 3 + p.smaps * 2 + p.heapProfiles * 1;
}

function processOptionLabel(p: ProcessOption): string {
  const parts: string[] = [];
  if (p.heapDumps > 0) parts.push(`${p.heapDumps} java_hprof`);
  if (p.heapProfiles > 0) parts.push(`${p.heapProfiles} heapprofd`);
  if (p.smaps > 0) parts.push(`${p.smaps} smaps`);
  return parts.length > 0 ? `${p.name} (${parts.join(', ')})` : p.name;
}

function buildSmapsChartData(
  trace: Trace,
  smapsSummaries: SmapsSummaryRow[],
): LineChartData | undefined {
  if (smapsSummaries.length === 0) return undefined;
  const dalvikPoints: {x: number; y: number}[] = [];
  const nativePoints: {x: number; y: number}[] = [];
  const otherPoints: {x: number; y: number}[] = [];
  for (const r of smapsSummaries) {
    const x = Number(r.ts - trace.traceInfo.start) / 1e9;
    dalvikPoints.push({x, y: r.dalvikAnonAndSwap});
    nativePoints.push({x, y: r.nativeAnonAndSwap});
    otherPoints.push({x, y: r.otherAnonAndSwap});
  }
  return {
    series: [
      {name: 'Java Heap', points: dalvikPoints},
      {name: 'Native Heap', points: nativePoints},
      {name: 'Other', points: otherPoints},
    ],
  };
}

interface GrowthTrend {
  firstTs: time;
  lastTs: time;
  firstSize: number;
  lastSize: number;
  delta: number;
  isGrowing: boolean;
}

// Compares the first and last value of a time-sorted series to detect growth.
// Returns undefined if there aren't at least two samples to establish a trend.
function analyseGrowth<T>(
  rows: T[],
  ts: (r: T) => time,
  value: (r: T) => number,
): GrowthTrend | undefined {
  if (rows.length < 2) return undefined;
  // rows are expected to be sorted by ts ascending.
  const first = rows[0];
  const last = rows[rows.length - 1];
  const delta = value(last) - value(first);
  return {
    firstTs: ts(first),
    lastTs: ts(last),
    firstSize: value(first),
    lastSize: value(last),
    delta,
    isGrowing: delta > 0,
  };
}

// Describes a growth trend as "grew/shrank by Δ (from X to Y) over Zs".
function describeGrowth(g: GrowthTrend): string {
  const verb = g.delta >= 0 ? 'grew' : 'shrank';
  const durationS = Duration.formatSeconds(Time.diff(g.lastTs, g.firstTs), 1);
  return `${verb} by ${formatBytes(Math.abs(g.delta))} over ${durationS}`;
}

// Does all the per-process data-prep up front: filter to the selected process,
// build the three chart datasets, sort the snapshot list, and precompute the
// delta maps the snapshot table needs. Pure function of (trace, data, process);
// the result is cached by the caller and reused across redraws.
function computeViewModel(
  trace: Trace,
  data: Data,
  processName?: string,
): ViewModel {
  const filteredData: Data = {
    heapDumps: data.heapDumps.filter((r) => r.processName === processName),
    heapDumpClasses: data.heapDumpClasses.filter(
      (r) => r.processName === processName,
    ),
    heapProfiles: data.heapProfiles.filter(
      (r) => r.processName === processName,
    ),
    javaHeapCounters: data.javaHeapCounters.filter(
      (r) => r.processName === processName,
    ),
    smapsSummaries: data.smapsSummaries.filter(
      (r) => r.processName === processName,
    ),
  };

  const hdChartData = buildHeapDumpsChartData(
    trace,
    filteredData.heapDumpClasses,
  );
  const npChartData = buildHeapProfilesChartData(
    trace,
    filteredData.heapProfiles,
  );
  const smapsChartData =
    filteredData.smapsSummaries.length > 1
      ? buildSmapsChartData(trace, filteredData.smapsSummaries)
      : undefined;

  // Δ in native unreleased since the previous heapprofd sample of the same
  // (process, heap). Keyed by eventId; absent for the first sample of a heap.
  const nativeDeltaByEventId = new Map<number, number>();
  const prevUnreleased = new Map<string, number>();
  for (const r of filteredData.heapProfiles) {
    const key = `${r.upid} - ${r.heapName}`;
    const prev = prevUnreleased.get(key);
    if (prev !== undefined) {
      nativeDeltaByEventId.set(r.eventId, r.unreleasedSize - prev);
    }
    prevUnreleased.set(key, r.unreleasedSize);
  }

  // Reachable Java heap bytes per dump (summed from the per-class rows), plus
  // the Δ vs the previous dump of the same process. Keyed by `upid-ts`.
  const reachableByDump = new Map<string, number>();
  for (const r of filteredData.heapDumpClasses) {
    if (!r.reachable) continue;
    const key = `${r.upid}-${r.ts}`;
    reachableByDump.set(key, (reachableByDump.get(key) ?? 0) + r.size);
  }
  const javaReachableDeltaByKey = new Map<string, number>();
  const prevReachable = new Map<number, number>();
  // Iterate dumps in ts order so "previous" is well-defined per process.
  for (const r of [...filteredData.heapDumps].sort((a, b) =>
    a.ts < b.ts ? -1 : a.ts > b.ts ? 1 : 0,
  )) {
    const key = `${r.upid}-${r.ts}`;
    const abs = reachableByDump.get(key) ?? 0;
    const prev = prevReachable.get(r.upid);
    if (prev !== undefined) {
      javaReachableDeltaByKey.set(key, abs - prev);
    }
    prevReachable.set(r.upid, abs);
  }

  return {
    filteredData,
    hdChartData,
    npChartData,
    smapsChartData,
    snapshots: getSortedSnapshots(filteredData),
    nativeDeltaByEventId,
    reachableByDump,
    javaReachableDeltaByKey,
  };
}

export class MemscopeLandingPage implements m.ClassComponent<{trace: Trace}> {
  private readonly dataSlot = new QuerySlot<Data>();
  private selectedProcessName?: string;

  // Memoized view model. The heavy data-prep (filtering, chart building, delta
  // maps) only depends on the loaded data and the selected process, neither of
  // which changes per redraw — so compute it once and reuse it until either
  // input changes. Without this, every mithril redraw (hover, mouse move, …)
  // re-derived everything and the page felt sluggish.
  private cachedViewModel?: ViewModel;
  private cachedForData?: Data;
  private cachedForProcess?: string;

  onremove() {
    this.dataSlot.dispose();
  }

  private getViewModel(
    trace: Trace,
    data: Data,
    processName?: string,
  ): ViewModel {
    if (
      this.cachedViewModel !== undefined &&
      this.cachedForData === data &&
      this.cachedForProcess === processName
    ) {
      return this.cachedViewModel;
    }
    const vm = computeViewModel(trace, data, processName);
    this.cachedViewModel = vm;
    this.cachedForData = data;
    this.cachedForProcess = processName;
    return vm;
  }

  view({attrs}: m.Vnode<{trace: Trace}>) {
    const {trace} = attrs;
    let result: QueryResult<Data> | undefined = undefined;
    let error: string | undefined;
    try {
      result = this.dataSlot.use({
        key: {traceId: trace.traceInfo.uuid},
        queryFn: () => loadData(trace),
      });
    } catch (e) {
      error = String(e);
    }

    let filteredSections: m.Children = null;
    if (result?.data) {
      const data = result.data;
      const processes = getUniqueProcesses(data);
      if (this.selectedProcessName === undefined && processes.length > 0) {
        // Pick the process with the most data, weighted by richness:
        // heap dumps > smaps > profiles.
        const best = processes.reduce((best, p) =>
          scoreProcess(p) > scoreProcess(best) ? p : best,
        );
        this.selectedProcessName = best.name;
      }

      const vm = this.getViewModel(trace, data, this.selectedProcessName);

      filteredSections = [
        processes.length > 1 &&
          m('.pf-memscope-process-select', [
            m('span.pf-memscope-process-select__label', 'Process'),
            m(
              Select,
              {
                value: this.selectedProcessName,
                onchange: (e: Event) => {
                  const val = (e.target as HTMLSelectElement).value;
                  this.selectedProcessName = val;
                },
              },
              processes.map((p) =>
                m('option', {value: p.name}, processOptionLabel(p)),
              ),
            ),
          ]),
        this.renderCaptureStrip(trace, vm.filteredData),
        this.renderSections(trace, vm),
      ];
    }

    return m(
      '.pf-memscope-landing',
      m(
        '.pf-memscope-landing__content',
        m('.pf-memscope-title-bar', m('h1', 'Memory Overview')),
        m(
          'p.pf-memscope-landing__subtitle',
          'Memory triage: smaps owns the total, the native and Java ' +
            'profilers explain what is inside.',
        ),
        error && m('p.pf-error', `Error: ${error}`),
        !error && result?.data === undefined && m('p', 'Loading…'),
        result?.data && filteredSections,
      ),
    );
  }

  // Header capture strip: trace identity plus one colored dot + terse facts
  // per source (smaps / java_hprof / heapprofd), for the selected process.
  private renderCaptureStrip(trace: Trace, data: Data): m.Children {
    const durationS = Number(trace.traceInfo.end - trace.traceInfo.start) / 1e9;

    const sources: {key: string; label: string; facts?: string}[] = [];

    const smapsSpan = sampleSpanSeconds(data.smapsSummaries);
    sources.push({
      key: 'smaps',
      label: 'smaps',
      facts:
        data.smapsSummaries.length > 0
          ? `${data.smapsSummaries.length} samples` +
            (smapsSpan !== undefined ? ` over ${smapsSpan.toFixed(1)}s` : '')
          : undefined,
    });

    sources.push({
      key: 'dump',
      label: 'java_hprof',
      facts:
        data.heapDumps.length > 0
          ? `${data.heapDumps.length} dumps`
          : undefined,
    });

    const nativeSpan = sampleSpanSeconds(data.heapProfiles);
    sources.push({
      key: 'native',
      label: 'heapprofd',
      facts:
        data.heapProfiles.length > 0
          ? `${data.heapProfiles.length} samples` +
            (nativeSpan !== undefined ? ` over ${nativeSpan.toFixed(1)}s` : '')
          : undefined,
    });

    return m('.pf-memscope-capture', [
      m('.pf-memscope-capture__identity', [
        m(
          'span.pf-memscope-capture__process',
          this.selectedProcessName ?? '<unknown>',
        ),
        m('span', `${trace.traceInfo.traceTitle} · ${durationS.toFixed(1)}s`),
      ]),
      m(
        '.pf-memscope-capture__sources',
        sources.map((s) =>
          m(
            'span.pf-memscope-capture__source',
            {
              className:
                s.facts === undefined
                  ? 'pf-memscope-capture__source--empty'
                  : undefined,
            },
            [
              m(
                `span.pf-memscope-capture__dot.pf-memscope-capture__dot--${s.key}`,
              ),
              m('span.pf-memscope-capture__label', s.label),
              m('span.pf-memscope-capture__facts', s.facts ?? 'none'),
            ],
          ),
        ),
      ),
    ]);
  }

  // Renders one problem row inside the Problems panel. `status` controls the
  // icon/colour: 'warning' = active smell, 'ok' = checked and healthy, 'none' =
  // the data needed for this check isn't present.
  private renderProblemCard(
    status: 'warning' | 'ok' | 'none',
    title: string,
    body: m.Children,
  ): m.Child {
    const icon =
      status === 'warning'
        ? 'info'
        : status === 'ok'
          ? 'check_circle'
          : 'remove_circle_outline';
    const intent =
      status === 'warning'
        ? Intent.Warning
        : status === 'ok'
          ? Intent.Success
          : Intent.None;
    return m(
      '.pf-memscope-problem',
      {className: `pf-memscope-problem--${status}`},
      m(Icon, {
        className: 'pf-memscope-problem__icon',
        icon,
        intent,
      }),
      m('.pf-memscope-problem__text', [
        m('.pf-memscope-problem__title', title),
        m('.pf-memscope-problem__detail', body),
      ]),
    );
  }

  private renderInsights(data: Data): m.Children {
    const cards: m.Child[] = [];

    // Problem 1: total footprint growing over time (smaps anon+swap).
    const growth = analyseGrowth(
      data.smapsSummaries,
      (r) => r.ts,
      (r) => r.anonAndSwap,
    );
    if (growth === undefined) {
      cards.push(
        this.renderProblemCard(
          'none',
          'Total footprint',
          'This process has no smaps samples.',
        ),
      );
    } else {
      const verb = growth.delta >= 0 ? 'grew' : 'shrank';
      const spanS = Number(growth.lastTs - growth.firstTs) / 1e9;
      cards.push(
        this.renderProblemCard(
          growth.isGrowing ? 'warning' : 'ok',
          growth.isGrowing
            ? 'Total footprint is growing'
            : 'Total footprint is stable or shrinking',
          [
            `Anon + swap ${verb} by ${formatBytes(Math.abs(growth.delta))} ` +
              `in ${spanS.toFixed(1)} seconds.`,
          ],
        ),
      );
    }

    // Problem 2: unreleased native memory growing.
    const nativeGrowth = analyseGrowth(
      data.heapProfiles,
      (r) => r.ts,
      (r) => r.unreleasedSize,
    );
    if (nativeGrowth === undefined) {
      cards.push(
        this.renderProblemCard(
          'none',
          'Unreleased native memory',
          'This process has no heapprofd profiles.',
        ),
      );
    } else {
      cards.push(
        this.renderProblemCard(
          nativeGrowth.isGrowing ? 'warning' : 'ok',
          nativeGrowth.isGrowing
            ? 'Unreleased native memory is growing'
            : 'Unreleased native memory is stable or shrinking',
          [`Native unreleased ${describeGrowth(nativeGrowth)}`],
        ),
      );
    }

    // Problem 3: java_hprof total bytes growing across dumps.
    const javaGrowth = analyseGrowth(
      data.heapDumps,
      (r) => r.ts,
      (r) => r.totalSize,
    );
    if (javaGrowth === undefined) {
      cards.push(
        this.renderProblemCard(
          'none',
          'java_hprof growth',
          'This process has no java_hprof dumps.',
        ),
      );
    } else {
      cards.push(
        this.renderProblemCard(
          javaGrowth.isGrowing ? 'warning' : 'ok',
          javaGrowth.isGrowing
            ? 'java_hprof heap is growing across dumps'
            : 'java_hprof heap is stable or shrinking',
          [`Heap total ${describeGrowth(javaGrowth)}`],
        ),
      );
    }

    if (cards.length === 0) {
      return null;
    }

    return m(
      Panel,
      {
        title: 'Insights',
        subtitle: 'Memory observations for the selected process.',
      },
      m('.pf-memscope-problems', cards),
    );
  }

  private renderSections(trace: Trace, vm: ViewModel): m.Children {
    const data = vm.filteredData;
    if (
      data.heapDumps.length === 0 &&
      data.heapProfiles.length === 0 &&
      data.javaHeapCounters.length === 0 &&
      data.smapsSummaries.length === 0
    ) {
      return m(
        '.pf-memscope-landing__empty',
        'No heap dumps or heap profile snapshots found in this trace.',
      );
    }

    const traceDurationS =
      Number(trace.traceInfo.end - trace.traceInfo.start) / 1e9;
    const {
      hdChartData,
      npChartData,
      smapsChartData,
      nativeDeltaByEventId,
      reachableByDump,
      javaReachableDeltaByKey,
    } = vm;

    return [
      m(
        '.pf-memscope-charts',
        smapsChartData &&
          m(
            Panel,
            {
              title: 'Process footprint (smaps)',
              subtitle:
                'Anonymous + swap memory — the honest total, by region ' +
                '(Java heap, native allocator heap, other). Not RSS: ' +
                'file-backed pages are excluded.',
            },
            m(LineChartSvg, {
              data: smapsChartData,
              height: 250,
              stacked: true,
              xAxisLabel: 'Time (s)',
              yAxisLabel: 'Size',
              showLegend: true,
              showPoints: true,
              gridLines: 'both',
              xAxisMin: 0,
              xAxisMax: traceDurationS,
              formatXValue: (v: number) => `${v.toFixed(1)}s`,
              formatYValue: (v: number) => formatBytes(v),
            }),
          ),
        npChartData &&
          m(
            Panel,
            {
              title: 'Native allocator (heapprofd)',
              subtitle:
                'Cumulative malloc memory, stacked: unreleased (live) + ' +
                'released (freed) = total allocated. Growing unreleased = ' +
                'leak; thick released = churn.',
            },
            m(LineChartSvg, {
              data: npChartData,
              height: 250,
              stacked: true,
              xAxisLabel: 'Time (s)',
              yAxisLabel: 'Size',
              showLegend: true,
              showPoints: true,
              gridLines: 'both',
              xAxisMin: 0,
              xAxisMax: traceDurationS,
              formatXValue: (v: number) => `${v.toFixed(1)}s`,
              formatYValue: (v: number) => formatBytes(v),
            }),
          ),
        hdChartData &&
          m(
            Panel,
            {
              title: 'Java heap composition (java_hprof)',
              subtitle:
                `Managed heap over time. Reachable objects split into the ` +
                `top ${HEAP_DUMP_TOP_CLASSES} classes by retained bytes ` +
                `(+ other); unreachable (uncollected garbage) on top.`,
            },
            m(LineChartSvg, {
              data: hdChartData,
              height: 250,
              stacked: true,
              xAxisLabel: 'Time (s)',
              yAxisLabel: 'Size',
              showLegend: true,
              showPoints: true,
              gridLines: 'both',
              xAxisMin: 0,
              xAxisMax: traceDurationS,
              formatXValue: (v: number) => `${v.toFixed(1)}s`,
              formatYValue: (v: number) => formatBytes(v),
            }),
          ),
        this.renderInsights(data),
      ),
      m(
        'table.pf-memscope-table',
        m(
          'thead',
          m(
            'tr',
            m('th', 'Time'),
            m('th', 'Type'),
            m('th', 'Heap'),
            m('th.pf-memscope-table__num', 'Smaps (anon+swap)'),
            m('th.pf-memscope-table__num', 'Java heap (reachable)'),
            m('th.pf-memscope-table__num', 'Native heap Δ'),
            m('th', ''),
            m('th', ''),
          ),
        ),
        m(
          'tbody',
          vm.snapshots.map((entry) => {
            if (entry.kind === 'dump') {
              const dump = entry.row;
              const viewOnTimeline = m(Button, {
                label: 'View on timeline',
                icon: 'timeline',
                onclick: () => {
                  const uri = trackUriFor(dump.upid, dump.eventType);
                  trace.selection.selectTrackEvent(uri, dump.eventId, {
                    scrollToSelection: true,
                  });
                  trace.navigate('#!/viewer');
                },
              });
              return m(
                'tr',
                m('td', m(Timestamp, {trace, ts: dump.ts})),
                m(
                  'td',
                  m(Chip, {
                    label: 'java_hprof',
                    intent: Intent.Primary,
                    rounded: true,
                    compact: true,
                  }),
                ),
                m('td', ''), // Heap
                m('td', ''), // Smaps (anon+swap)
                (() => {
                  const key = `${dump.upid}-${dump.ts}`;
                  const abs = reachableByDump.get(key) ?? 0;
                  const delta = javaReachableDeltaByKey.get(key);
                  return m(
                    'td.pf-memscope-table__num.pf-memscope-table__size',
                    m('div', formatBytes(abs)),
                    delta !== undefined &&
                      m('div.pf-memscope-table__delta', formatDelta(delta)),
                  );
                })(), // Java heap (reachable)
                m('td', ''), // Native heap Δ
                m('td', viewOnTimeline),
                m(
                  'td',
                  m(Button, {
                    label: 'Open in Heap Dump Explorer',
                    icon: 'memory',
                    onclick: () => {
                      trace.navigate(
                        `#!/heapdump?upid=${dump.upid}&ts=${dump.ts}`,
                      );
                    },
                  }),
                ),
              );
            }
            if (entry.kind === 'smaps') {
              const smaps = entry.row;
              const viewOnTimeline = m(Button, {
                label: 'View on timeline',
                icon: 'timeline',
                onclick: () => {
                  trace.scrollTo({
                    time: {start: smaps.ts, behavior: 'pan'},
                    track:
                      smaps.trackId !== undefined
                        ? {uri: `/counter_${smaps.trackId}`, expandGroup: true}
                        : undefined,
                  });
                  trace.navigate('#!/viewer');
                },
              });
              return m(
                'tr',
                m('td', m(Timestamp, {trace, ts: smaps.ts})),
                m(
                  'td',
                  m(Chip, {
                    label: 'smaps',
                    intent: Intent.None,
                    rounded: true,
                    compact: true,
                  }),
                ),
                m('td', ''), // Heap
                m(
                  'td.pf-memscope-table__num.pf-memscope-table__size',
                  formatBytes(smaps.anonAndSwap),
                ), // Smaps (anon+swap)
                m('td', ''), // Java heap (reachable)
                m('td', ''), // Native heap Δ
                m('td', viewOnTimeline),
                m('td', ''),
              );
            }
            const profile = entry.row;
            const viewOnTimeline = m(Button, {
              label: 'View on timeline',
              icon: 'timeline',
              onclick: () => {
                const uri = trackUriFor(profile.upid, profile.eventType);
                trace.selection.selectTrackEvent(uri, profile.eventId, {
                  scrollToSelection: true,
                });
                trace.navigate('#!/viewer');
              },
            });
            return m(
              'tr',
              m('td', m(Timestamp, {trace, ts: profile.ts})),
              m(
                'td',
                m(Chip, {
                  label: 'heapprofd',
                  intent: Intent.Success,
                  rounded: true,
                  compact: true,
                }),
              ),
              m('td', profile.heapName), // Heap
              m('td', ''), // Smaps (anon+swap)
              m('td', ''), // Java heap (reachable)
              (() => {
                const delta = nativeDeltaByEventId.get(profile.eventId);
                return m(
                  'td.pf-memscope-table__num.pf-memscope-table__size',
                  m('div', delta !== undefined ? formatDelta(delta) : '—'),
                  m(
                    'div.pf-memscope-table__delta',
                    `${profile.samples.toLocaleString()} samples`,
                  ),
                );
              })(), // Native heap Δ
              m('td', viewOnTimeline),
              m('td', ''),
            );
          }),
        ),
      ),
    ];
  }
}
