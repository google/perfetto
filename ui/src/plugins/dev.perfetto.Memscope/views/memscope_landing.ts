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
import {Time, type time} from '../../../base/time';
import type {Trace} from '../../../public/trace';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../../trace_processor/query_result';
import {Icon} from '../../../widgets/icon';
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

// Per-(snapshot, category) smaps aggregate, with the resident memory broken
// down into private dirty / private clean / shared so blocks can be shaded by
// reclaimability. Categories are the keys of SMAPS_CATEGORIES.
interface SmapsCategoryRow {
  ts: time;
  upid: number;
  processName: string;
  category: string;
  rss: number;
  swap: number;
  anon: number;
  privateDirty: number;
  privateClean: number;
  shared: number;
}

// Per-dump heap graph rollup from android_heap_graph_stats: totals,
// reachability split and the process state (uptime / OOM score) at dump time.
interface HeapGraphStatsRow {
  ts: time;
  upid: number;
  processName: string;
  processUptime?: number; // ns
  totalHeapSize: number;
  totalNativeSize: number;
  totalObjCount: number;
  reachableHeapSize: number;
  reachableNativeSize: number;
  reachableObjCount: number;
  oomScoreAdj?: number;
  anonRssAndSwap?: number;
}

// Per-class aggregation (last dump per process only) from
// android_heap_graph_class_aggregation. rnRetained/rnDominated/rnCount are
// the class's rank under each of the three orderings the tables use.
interface HeapClassAggRow {
  ts: time;
  upid: number;
  processName: string;
  typeName: string;
  objCount: number;
  sizeBytes: number;
  nativeSizeBytes: number;
  reachableObjCount: number;
  reachableSizeBytes: number;
  reachableNativeSizeBytes: number;
  dominatedObjCount: number;
  dominatedSizeBytes: number;
  dominatedNativeSizeBytes: number;
  rnRetained: number;
  rnDominated: number;
  rnCount: number;
}

// Reachable android.graphics.Bitmap instances grouped by (dimensions,
// storage backing), last dump per process. width/height are NULL on
// proto-format heap graphs (only ART HPROF dumps record field values).
interface BitmapGroupRow {
  ts: time;
  upid: number;
  processName: string;
  width?: number;
  height?: number;
  storage?: string;
  count: number;
  selfSize: number;
  nativeSize: number;
}

// One top-unreleased heapprofd callsite with its full frame chain
// (leaf → root), assembled from the recursive callsite walk.
interface NativeStackRow {
  upid: number;
  processName: string;
  unreleased: number;
  allocs: number;
  frames: string[];
}

// Max value of a per-process memory counter (e.g. mem.rss.watermark).
interface CounterMaxRow {
  processName: string;
  counterName: string;
  maxValue: number;
}

interface ThreadCountRow {
  processName: string;
  count: number;
}

// One smaps path (VMA group) at the last snapshot of a process, with the
// full residency split. Powers the "Smaps detail" tab and the ART overhead
// card.
interface SmapsPathRow {
  ts: time;
  upid: number;
  processName: string;
  path: string;
  category: string;
  rss: number;
  pss: number;
  anon: number;
  swap: number;
  privateDirty: number;
  privateClean: number;
  sharedDirty: number;
  sharedClean: number;
}

interface Data {
  heapDumps: HeapDumpRow[];
  heapProfiles: HeapProfileRow[];
  javaHeapCounters: JavaHeapCounterRow[];
  smapsSummaries: SmapsSummaryRow[];
  smapsCategories: SmapsCategoryRow[];
  heapStats: HeapGraphStatsRow[];
  heapClasses: HeapClassAggRow[];
  bitmaps: BitmapGroupRow[];
  nativeStacks: NativeStackRow[];
  counterMaxima: CounterMaxRow[];
  threadCounts: ThreadCountRow[];
  smapsPaths: SmapsPathRow[];
}

// Everything derived from (raw data, selected process) that doesn't depend on
// per-frame state. Computed once per selection in computeViewModel() and reused
// across redraws — the data is immutable after load, so none of this changes
// until the user picks a different process.
interface ViewModel {
  filteredData: Data;
  // Memory map ("Where did all the memory go?") panel inputs.
  smapsSnapshots: SmapsSnapshot[];
  compChartData?: LineChartData;
}

// Path → category classification shared by the per-category rollup and the
// smaps detail query. Keys match SMAPS_CATEGORIES.
const SMAPS_CATEGORY_CASE_SQL = `
      CASE
        WHEN s.path GLOB '*dalvik*' OR s.path GLOB '/dev/ashmem/dalvik*'
          THEN 'java'
        WHEN s.path GLOB '[anon:scudo*' OR s.path GLOB '[anon:libc_malloc*'
          OR s.path GLOB '[anon:jemalloc*' OR s.path GLOB '[anon:GWP-ASan*'
          OR s.path = '[heap]'
          THEN 'native'
        WHEN s.path = '[stack]' OR s.path GLOB '[anon:stack*'
          THEN 'stack'
        WHEN s.path GLOB '/dev/kgsl*' OR s.path GLOB '/dev/mali*'
          OR s.path GLOB '/dev/dri*' OR s.path GLOB '*dmabuf*'
          THEN 'graphics'
        WHEN s.path GLOB '/*'
          THEN 'file'
        ELSE 'other'
      END`;

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

  // Per-(snapshot, category) smaps rollup for the memory map panel. Same path
  // classification as the summary query above, extended with graphics, thread
  // stacks and file-backed buckets, plus the private/shared residency split.
  const smapsCategories: SmapsCategoryRow[] = [];
  const catRes = await trace.engine.query(`
    SELECT
      s.ts AS ts,
      MIN(p.upid) AS upid,
      coalesce(p.name, '<unknown>') AS pname,
      ${SMAPS_CATEGORY_CASE_SQL} AS category,
      CAST(ifnull(SUM(s.rss_kb), 0) * 1024 AS INT) AS rss,
      CAST(ifnull(SUM(s.swap_kb), 0) * 1024 AS INT) AS swap,
      CAST(ifnull(SUM(s.anonymous_kb), 0) * 1024 AS INT) AS anon,
      CAST(ifnull(SUM(s.private_dirty_kb), 0) * 1024 AS INT)
        AS private_dirty,
      CAST(ifnull(SUM(s.private_clean_resident_kb), 0) * 1024 AS INT)
        AS private_clean,
      CAST(ifnull(SUM(
        s.shared_dirty_resident_kb + s.shared_clean_resident_kb
      ), 0) * 1024 AS INT) AS shared
    FROM profiler_smaps s
    LEFT JOIN process p USING (upid)
    GROUP BY s.ts, p.name, category
    ORDER BY s.ts ASC
  `);
  for (
    const it = catRes.iter({
      ts: LONG,
      upid: NUM,
      pname: STR,
      category: STR,
      rss: NUM,
      swap: NUM,
      anon: NUM,
      private_dirty: NUM,
      private_clean: NUM,
      shared: NUM,
    });
    it.valid();
    it.next()
  ) {
    smapsCategories.push({
      ts: Time.fromRaw(it.ts),
      upid: it.upid,
      processName: it.pname,
      category: it.category,
      rss: it.rss,
      swap: it.swap,
      anon: it.anon,
      privateDirty: it.private_dirty,
      privateClean: it.private_clean,
      shared: it.shared,
    });
  }

  // Per-dump heap graph stats (uptime, OOM score, reachability totals).
  // Cheap: a single grouped scan of heap_graph_object.
  const heapStats: HeapGraphStatsRow[] = [];
  await trace.engine.query(
    'INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_stats;',
  );
  const statsRes = await trace.engine.query(`
    SELECT
      s.graph_sample_ts AS ts,
      s.upid AS upid,
      coalesce(p.cmdline, p.name, '<unknown>') AS pname,
      s.process_uptime AS process_uptime,
      s.total_heap_size AS total_heap_size,
      s.total_native_alloc_registry_size AS total_native_size,
      s.total_obj_count AS total_obj_count,
      s.reachable_heap_size AS reachable_heap_size,
      s.reachable_native_alloc_registry_size AS reachable_native_size,
      s.reachable_obj_count AS reachable_obj_count,
      s.oom_score_adj AS oom_score_adj,
      s.anon_rss_and_swap_size AS anon_rss_and_swap
    FROM android_heap_graph_stats s
    JOIN process p USING (upid)
    ORDER BY s.graph_sample_ts ASC
  `);
  for (
    const it = statsRes.iter({
      ts: LONG,
      upid: NUM,
      pname: STR,
      process_uptime: NUM_NULL,
      total_heap_size: NUM,
      total_native_size: NUM,
      total_obj_count: NUM,
      reachable_heap_size: NUM,
      reachable_native_size: NUM,
      reachable_obj_count: NUM,
      oom_score_adj: NUM_NULL,
      anon_rss_and_swap: NUM_NULL,
    });
    it.valid();
    it.next()
  ) {
    heapStats.push({
      ts: Time.fromRaw(it.ts),
      upid: it.upid,
      processName: it.pname,
      processUptime: it.process_uptime ?? undefined,
      totalHeapSize: it.total_heap_size,
      totalNativeSize: it.total_native_size,
      totalObjCount: it.total_obj_count,
      reachableHeapSize: it.reachable_heap_size,
      reachableNativeSize: it.reachable_native_size,
      reachableObjCount: it.reachable_obj_count,
      oomScoreAdj: it.oom_score_adj ?? undefined,
      anonRssAndSwap: it.anon_rss_and_swap ?? undefined,
    });
  }

  // Per-class aggregation for the last dump of each process, keeping only
  // classes ranked in the top 8 by retained, dominated or instance count —
  // that's all the three "top 5" tables can ever show.
  const heapClasses: HeapClassAggRow[] = [];
  await trace.engine.query(
    'INCLUDE PERFETTO MODULE ' +
      'android.memory.heap_graph.heap_graph_class_aggregation;',
  );
  const aggRes = await trace.engine.query(`
    WITH last_dump AS (
      SELECT upid, MAX(graph_sample_ts) AS ts
      FROM android_heap_graph_class_aggregation
      GROUP BY upid
    ),
    ranked AS (
      SELECT
        a.*,
        ROW_NUMBER() OVER (
          PARTITION BY a.upid
          ORDER BY a.reachable_size_bytes + a.reachable_native_size_bytes DESC
        ) AS rn_retained,
        ROW_NUMBER() OVER (
          PARTITION BY a.upid
          ORDER BY a.dominated_size_bytes + a.dominated_native_size_bytes DESC
        ) AS rn_dominated,
        ROW_NUMBER() OVER (
          PARTITION BY a.upid ORDER BY a.obj_count DESC
        ) AS rn_count
      FROM android_heap_graph_class_aggregation a
      JOIN last_dump l ON a.upid = l.upid AND a.graph_sample_ts = l.ts
    )
    SELECT
      r.graph_sample_ts AS ts,
      r.upid AS upid,
      coalesce(p.cmdline, p.name, '<unknown>') AS pname,
      r.type_name AS type_name,
      r.obj_count AS obj_count,
      r.size_bytes AS size_bytes,
      r.native_size_bytes AS native_size_bytes,
      r.reachable_obj_count AS reachable_obj_count,
      r.reachable_size_bytes AS reachable_size_bytes,
      r.reachable_native_size_bytes AS reachable_native_size_bytes,
      r.dominated_obj_count AS dominated_obj_count,
      r.dominated_size_bytes AS dominated_size_bytes,
      r.dominated_native_size_bytes AS dominated_native_size_bytes,
      r.rn_retained AS rn_retained,
      r.rn_dominated AS rn_dominated,
      r.rn_count AS rn_count
    FROM ranked r
    JOIN process p USING (upid)
    WHERE r.rn_retained <= 8 OR r.rn_dominated <= 8 OR r.rn_count <= 8
  `);
  for (
    const it = aggRes.iter({
      ts: LONG,
      upid: NUM,
      pname: STR,
      type_name: STR,
      obj_count: NUM,
      size_bytes: NUM,
      native_size_bytes: NUM,
      reachable_obj_count: NUM,
      reachable_size_bytes: NUM,
      reachable_native_size_bytes: NUM,
      dominated_obj_count: NUM,
      dominated_size_bytes: NUM,
      dominated_native_size_bytes: NUM,
      rn_retained: NUM,
      rn_dominated: NUM,
      rn_count: NUM,
    });
    it.valid();
    it.next()
  ) {
    heapClasses.push({
      ts: Time.fromRaw(it.ts),
      upid: it.upid,
      processName: it.pname,
      typeName: it.type_name,
      objCount: it.obj_count,
      sizeBytes: it.size_bytes,
      nativeSizeBytes: it.native_size_bytes,
      reachableObjCount: it.reachable_obj_count,
      reachableSizeBytes: it.reachable_size_bytes,
      reachableNativeSizeBytes: it.reachable_native_size_bytes,
      dominatedObjCount: it.dominated_obj_count,
      dominatedSizeBytes: it.dominated_size_bytes,
      dominatedNativeSizeBytes: it.dominated_native_size_bytes,
      rnRetained: it.rn_retained,
      rnDominated: it.rn_dominated,
      rnCount: it.rn_count,
    });
  }

  // Reachable Bitmaps grouped by (dimensions, storage), last dump per
  // process. Dimensions are NULL on proto-format heap graphs.
  const bitmaps: BitmapGroupRow[] = [];
  await trace.engine.query(
    'INCLUDE PERFETTO MODULE android.memory.heap_graph.bitmap;',
  );
  const bmpRes = await trace.engine.query(`
    WITH last_dump AS (
      SELECT upid, MAX(graph_sample_ts) AS ts
      FROM heap_graph_bitmaps
      GROUP BY upid
    )
    SELECT
      b.graph_sample_ts AS ts,
      b.upid AS upid,
      coalesce(p.cmdline, p.name, '<unknown>') AS pname,
      b.width AS width,
      b.height AS height,
      b.bitmap_storage_type AS storage,
      COUNT(*) AS cnt,
      CAST(ifnull(SUM(b.self_size), 0) AS INT) AS self_size,
      CAST(ifnull(SUM(b.native_size), 0) AS INT) AS native_size
    FROM heap_graph_bitmaps b
    JOIN last_dump l ON b.upid = l.upid AND b.graph_sample_ts = l.ts
    JOIN process p USING (upid)
    WHERE b.reachable
    GROUP BY b.upid, b.width, b.height, b.bitmap_storage_type
  `);
  for (
    const it = bmpRes.iter({
      ts: LONG,
      upid: NUM,
      pname: STR,
      width: NUM_NULL,
      height: NUM_NULL,
      storage: STR_NULL,
      cnt: NUM,
      self_size: NUM,
      native_size: NUM,
    });
    it.valid();
    it.next()
  ) {
    bitmaps.push({
      ts: Time.fromRaw(it.ts),
      upid: it.upid,
      processName: it.pname,
      width: it.width ?? undefined,
      height: it.height ?? undefined,
      storage: it.storage ?? undefined,
      count: it.cnt,
      selfSize: it.self_size,
      nativeSize: it.native_size,
    });
  }

  // Top unreleased heapprofd callsites per process, with their frame chains
  // (depth 0 = leaf). Assembled into NativeStackRow below.
  const nativeStacks: NativeStackRow[] = [];
  const stackRes = await trace.engine.query(`
    WITH unrel AS (
      SELECT
        a.upid AS upid,
        a.callsite_id AS callsite_id,
        SUM(a.size) AS unreleased,
        SUM(CASE WHEN a.count > 0 THEN a.count ELSE 0 END) AS allocs
      FROM heap_profile_allocation a
      GROUP BY a.upid, a.callsite_id
      HAVING SUM(a.size) > 0
    ),
    top_sites AS (
      SELECT * FROM (
        SELECT
          u.*,
          ROW_NUMBER() OVER (
            PARTITION BY u.upid ORDER BY u.unreleased DESC
          ) AS rn
        FROM unrel u
      ) WHERE rn <= 5
    ),
    chain(top_id, upid, unreleased, allocs, callsite_id, depth) AS (
      SELECT callsite_id, upid, unreleased, allocs, callsite_id, 0
      FROM top_sites
      UNION ALL
      SELECT c.top_id, c.upid, c.unreleased, c.allocs, sc.parent_id,
        c.depth + 1
      FROM chain c
      JOIN stack_profile_callsite sc ON sc.id = c.callsite_id
      WHERE sc.parent_id IS NOT NULL AND c.depth < 128
    )
    SELECT
      c.top_id AS top_id,
      c.upid AS upid,
      coalesce(p.cmdline, p.name, '<unknown>') AS pname,
      CAST(c.unreleased AS INT) AS unreleased,
      CAST(c.allocs AS INT) AS allocs,
      c.depth AS depth,
      coalesce(f.deobfuscated_name, f.name, '<unknown>') AS frame_name
    FROM chain c
    JOIN stack_profile_callsite sc ON sc.id = c.callsite_id
    JOIN stack_profile_frame f ON sc.frame_id = f.id
    JOIN process p ON c.upid = p.upid
    ORDER BY c.upid, c.top_id, c.depth ASC
  `);
  {
    let cur: NativeStackRow | undefined;
    let curKey = '';
    for (
      const it = stackRes.iter({
        top_id: NUM,
        upid: NUM,
        pname: STR,
        unreleased: NUM,
        allocs: NUM,
        depth: NUM,
        frame_name: STR,
      });
      it.valid();
      it.next()
    ) {
      const key = `${it.upid}-${it.top_id}`;
      if (key !== curKey) {
        cur = {
          upid: it.upid,
          processName: it.pname,
          unreleased: it.unreleased,
          allocs: it.allocs,
          frames: [],
        };
        nativeStacks.push(cur);
        curKey = key;
      }
      cur!.frames.push(it.frame_name);
    }
  }
  nativeStacks.sort((a, b) => b.unreleased - a.unreleased);

  // Maxima of the cheap polled memory counters (for the RSS spike card).
  const counterMaxima: CounterMaxRow[] = [];
  const maxRes = await trace.engine.query(`
    SELECT
      coalesce(p.name, '<unknown>') AS pname,
      t.name AS counter_name,
      MAX(c.value) AS max_value
    FROM counter c
    JOIN process_counter_track t ON c.track_id = t.id
    JOIN process p USING (upid)
    WHERE t.name IN ('mem.rss.watermark', 'mem.rss')
    GROUP BY p.name, t.name
  `);
  for (
    const it = maxRes.iter({pname: STR, counter_name: STR, max_value: NUM});
    it.valid();
    it.next()
  ) {
    counterMaxima.push({
      processName: it.pname,
      counterName: it.counter_name,
      maxValue: it.max_value,
    });
  }

  // Thread counts (for the thread stacks card).
  const threadCounts: ThreadCountRow[] = [];
  const thrRes = await trace.engine.query(`
    SELECT
      coalesce(p.name, '<unknown>') AS pname,
      COUNT(*) AS cnt
    FROM thread t
    JOIN process p USING (upid)
    GROUP BY p.name
  `);
  for (const it = thrRes.iter({pname: STR, cnt: NUM}); it.valid(); it.next()) {
    threadCounts.push({processName: it.pname, count: it.cnt});
  }

  // Path-level smaps rows at the last snapshot of each process, for the
  // smaps detail tab and the ART overhead card.
  const smapsPaths: SmapsPathRow[] = [];
  const pathRes = await trace.engine.query(`
    WITH last_ts AS (
      SELECT upid, MAX(ts) AS ts FROM profiler_smaps GROUP BY upid
    )
    SELECT
      s.ts AS ts,
      s.upid AS upid,
      coalesce(p.name, '<unknown>') AS pname,
      s.path AS path,
      ${SMAPS_CATEGORY_CASE_SQL} AS category,
      CAST(ifnull(SUM(s.rss_kb), 0) * 1024 AS INT) AS rss,
      CAST(ifnull(SUM(s.proportional_resident_kb), 0) * 1024 AS INT) AS pss,
      CAST(ifnull(SUM(s.anonymous_kb), 0) * 1024 AS INT) AS anon,
      CAST(ifnull(SUM(s.swap_kb), 0) * 1024 AS INT) AS swap,
      CAST(ifnull(SUM(s.private_dirty_kb), 0) * 1024 AS INT)
        AS private_dirty,
      CAST(ifnull(SUM(s.private_clean_resident_kb), 0) * 1024 AS INT)
        AS private_clean,
      CAST(ifnull(SUM(s.shared_dirty_resident_kb), 0) * 1024 AS INT)
        AS shared_dirty,
      CAST(ifnull(SUM(s.shared_clean_resident_kb), 0) * 1024 AS INT)
        AS shared_clean
    FROM profiler_smaps s
    JOIN last_ts l ON s.upid = l.upid AND s.ts = l.ts
    LEFT JOIN process p USING (upid)
    GROUP BY s.upid, s.path
    ORDER BY rss DESC
  `);
  for (
    const it = pathRes.iter({
      ts: LONG,
      upid: NUM,
      pname: STR,
      path: STR,
      category: STR,
      rss: NUM,
      pss: NUM,
      anon: NUM,
      swap: NUM,
      private_dirty: NUM,
      private_clean: NUM,
      shared_dirty: NUM,
      shared_clean: NUM,
    });
    it.valid();
    it.next()
  ) {
    smapsPaths.push({
      ts: Time.fromRaw(it.ts),
      upid: it.upid,
      processName: it.pname,
      path: it.path,
      category: it.category,
      rss: it.rss,
      pss: it.pss,
      anon: it.anon,
      swap: it.swap,
      privateDirty: it.private_dirty,
      privateClean: it.private_clean,
      sharedDirty: it.shared_dirty,
      sharedClean: it.shared_clean,
    });
  }

  return {
    heapDumps,
    heapProfiles,
    javaHeapCounters,
    smapsSummaries,
    smapsCategories,
    heapStats,
    heapClasses,
    bitmaps,
    nativeStacks,
    counterMaxima,
    threadCounts,
    smapsPaths,
  };
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

// Trims a fully-qualified class name to its last segment(s) for legends, e.g.
// 'java.util.HashMap$Node' -> 'HashMap$Node'. Arrays and 'Other' pass through.
function shortClassName(name: string): string {
  if (name === 'Other') return name;
  const arrayDepth = (name.match(/\[\]/g) ?? []).length;
  const base = name.replace(/\[\]/g, '');
  const last = base.slice(base.lastIndexOf('.') + 1);
  return last + '[]'.repeat(arrayDepth);
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

// Display metadata for the smaps categories, in stack/legend order. The keys
// match the `category` values produced by the smaps category query.
const SMAPS_CATEGORIES = [
  {key: 'native', label: 'Native', color: '#4285f4'},
  {key: 'java', label: 'Java', color: '#f4b400'},
  {key: 'file', label: 'File-backed', color: '#34a853'},
  {key: 'graphics', label: 'Graphics', color: '#a142f4'},
  {key: 'stack', label: 'Thread stacks', color: '#26c6da'},
  {key: 'other', label: 'Other', color: '#9aa0a6'},
];

const MEMMAP_GREY = '#9aa0a6';

// Two-level taxonomy for the smaps detail tree: top-level groups and their
// subcategories, in display order. classifyMapping() assigns each path.
const SMAPS_TREE: {
  key: string;
  label: string;
  color: string;
  subs: {key: string; label: string; color: string}[];
}[] = [
  {
    key: 'anon',
    label: 'Anonymous',
    color: '#4285f4',
    subs: [
      {key: 'native_heap', label: 'Native heap', color: '#4285f4'},
      {key: 'java_heap', label: 'Java heap', color: '#f4b400'},
      {key: 'java_other', label: 'Java other / ART', color: '#f4b400'},
      {key: 'stacks', label: 'Thread stacks', color: '#26c6da'},
      {key: 'other_anon', label: 'Other anon', color: MEMMAP_GREY},
    ],
  },
  {
    key: 'file',
    label: 'File-backed',
    color: '#34a853',
    subs: [
      {key: 'java_code', label: 'Java (.jar/.oat/.art)', color: '#34a853'},
      {key: 'so_libs', label: 'Native libs (.so)', color: '#34a853'},
      {key: 'resources', label: 'Resources / APK', color: '#34a853'},
      {key: 'other_file', label: 'Other file-backed', color: '#34a853'},
    ],
  },
  {
    key: 'gfx',
    label: 'Graphics / shared',
    color: '#a142f4',
    subs: [
      {key: 'ashmem', label: 'ashmem / dmabuf', color: '#a142f4'},
      {key: 'gpu', label: 'GPU / driver', color: '#a142f4'},
    ],
  },
];

// Classifies one smaps path into a (group, sub) of SMAPS_TREE. Order
// matters: graphics devices before the generic file-backed '/' check, and
// the dalvik heap spaces before the generic dalvik bucket.
function classifyMapping(path: string): {group: string; sub: string} {
  if (/dmabuf|^\/dev\/ashmem/.test(path)) {
    return {group: 'gfx', sub: 'ashmem'};
  }
  if (/^\/dev\/(kgsl|mali|dri)/.test(path)) {
    return {group: 'gfx', sub: 'gpu'};
  }
  if (path.startsWith('/')) {
    if (/\.so$/.test(path)) return {group: 'file', sub: 'so_libs'};
    if (/\.(jar|dex|oat|odex|vdex|art)$/.test(path)) {
      return {group: 'file', sub: 'java_code'};
    }
    if (/\.(apk|ttf|otf|dat)$/.test(path) || path.startsWith('/fonts/')) {
      return {group: 'file', sub: 'resources'};
    }
    return {group: 'file', sub: 'other_file'};
  }
  if (
    /^\[anon:dalvik-(main|large object|zygote|non moving|free list)/.test(path)
  ) {
    return {group: 'anon', sub: 'java_heap'};
  }
  if (/dalvik|\.art\]$/.test(path)) return {group: 'anon', sub: 'java_other'};
  if (/^\[stack\]|^\[anon:stack/.test(path)) {
    return {group: 'anon', sub: 'stacks'};
  }
  if (
    /^\[anon:(scudo|libc_malloc|jemalloc|GWP-ASan|partition_alloc|\.bss)/.test(
      path,
    ) ||
    path === '[heap]'
  ) {
    return {group: 'anon', sub: 'native_heap'};
  }
  return {group: 'anon', sub: 'other_anon'};
}

// Plain-language explanation shown under the block breakdown while hovering
// each block of the memory map.
const MEMMAP_BLOCK_INFO: Record<string, string> = {
  'File-backed':
    'Memory backed by files on disk: code (.so/.oat), fonts and resources. ' +
    'Mostly clean — the kernel can drop and re-read it under pressure.',
  'Anonymous':
    'Memory not backed by any file: heaps and runtime allocations. This is ' +
    'what your process truly costs; it can only be reclaimed by swapping.',
  'Graphics':
    'GPU buffers and driver mappings (kgsl / mali / dri / dmabuf) mapped ' +
    'into this process.',
  'Other':
    'Non-anonymous regions that do not fit the other buckets (devices, ' +
    'shared memory files, …).',
  'Native':
    'Native allocator arenas (scudo / jemalloc / libc malloc / GWP-ASan) ' +
    'and the legacy [heap]. What malloc() costs you in resident memory.',
  'Java':
    'Dalvik/ART managed heap and runtime spaces — everything the Java ' +
    'garbage collector manages.',
  'Thread stacks': 'Stack memory for every live thread in the process.',
  'Other anon':
    'Anonymous memory not attributable to a specific allocator: plain ' +
    'mmap() regions, untagged buffers, IPC shared memory.',
};

// "4h 12m" / "5m 3s" / "37s" from a duration in nanoseconds.
function formatDurationShort(ns: number): string {
  const secs = ns / 1e9;
  if (secs < 60) return `${secs.toFixed(0)}s`;
  const mins = secs / 60;
  if (mins < 60) return `${Math.floor(mins)}m ${Math.round(secs % 60)}s`;
  return `${Math.floor(mins / 60)}h ${Math.round(mins % 60)}m`;
}

// Coarse human label for an oom_score_adj value.
function oomScoreLabel(adj: number): string {
  if (adj <= 0) return 'foreground';
  if (adj <= 200) return 'perceptible';
  if (adj <= 600) return 'service';
  return 'cached';
}

// Frames elided from native callstack snippets: allocator internals,
// unwinder and libc plumbing that carry no signal about the caller.
const BORING_FRAME_RE =
  /art::|libunwind|__libc|scudo::|je_|tcmalloc|std::__|__gnu|__cxa|_ZnwmSt|^\[/;

// Shrinks a full frame chain (leaf → root) to the mock's "snippet" form:
// the allocator leaf, the next few interesting frames, then a couple of
// frames from the root end to anchor the stack in its thread/task.
function stackSnippet(frames: string[]): string[] {
  if (frames.length <= 7) return frames;
  const leaf = frames[0];
  const middle = frames
    .slice(1, -2)
    .filter((f) => !BORING_FRAME_RE.test(f))
    .slice(0, 4);
  const root = frames.slice(-2);
  return [leaf, ...middle, '…', ...root];
}

// One smaps snapshot (a single ts) with its per-category rows and the total
// resident + swap footprint.
interface SmapsSnapshot {
  ts: time;
  byCategory: Map<string, SmapsCategoryRow>;
  total: number;
}

function buildSmapsSnapshots(rows: SmapsCategoryRow[]): SmapsSnapshot[] {
  const byTs = new Map<time, Map<string, SmapsCategoryRow>>();
  for (const r of rows) {
    let cats = byTs.get(r.ts);
    if (!cats) {
      cats = new Map();
      byTs.set(r.ts, cats);
    }
    cats.set(r.category, r);
  }
  return Array.from(byTs.entries())
    .sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0))
    .map(([ts, byCategory]) => {
      let total = 0;
      for (const r of byCategory.values()) total += r.rss + r.swap;
      return {ts, byCategory, total};
    });
}

// Stacked composition-over-time chart: one series per smaps category, value =
// resident + swap. Categories that are zero everywhere are dropped.
function buildCompositionChartData(
  trace: Trace,
  snapshots: SmapsSnapshot[],
): LineChartData | undefined {
  if (snapshots.length === 0) return undefined;
  const series = SMAPS_CATEGORIES.map((c) => ({
    name: c.label,
    color: c.color,
    points: snapshots.map((s) => {
      const r = s.byCategory.get(c.key);
      return {
        x: Number(s.ts - trace.traceInfo.start) / 1e9,
        y: r ? r.rss + r.swap : 0,
      };
    }),
  })).filter((s) => s.points.some((p) => p.y > 0));
  return {series};
}

// One block in the snapshot breakdown bar. `color === undefined` renders an
// invisible spacer (used to indent row 2 under the "Anonymous" block). When
// the dirtySwap/clean/shared split is present the block is shaded by it.
interface MemBlock {
  label: string;
  bytes: number;
  color?: string;
  dirtySwap?: number;
  clean?: number;
  shared?: number;
}

function smapsCategoryColor(key: string): string {
  return SMAPS_CATEGORIES.find((c) => c.key === key)?.color ?? MEMMAP_GREY;
}

// Builds the two-level block breakdown for one snapshot. Row 1 splits the
// total into file-backed / anonymous / graphics / other; row 2 splits the
// anonymous block into native / Java / thread stacks / other anon. Both rows
// sum to the snapshot total, so widths are directly comparable.
function buildMemBlocks(snap: SmapsSnapshot): {
  row1: MemBlock[];
  row2: MemBlock[];
} {
  const get = (key: string) => snap.byCategory.get(key);
  const resSwap = (r?: SmapsCategoryRow) => (r ? r.rss + r.swap : 0);
  const comps = (rows: (SmapsCategoryRow | undefined)[]) => {
    let dirtySwap = 0;
    let clean = 0;
    let shared = 0;
    for (const r of rows) {
      if (!r) continue;
      dirtySwap += r.privateDirty + r.swap;
      clean += r.privateClean;
      shared += r.shared;
    }
    return {dirtySwap, clean, shared};
  };

  const file = get('file');
  const graphics = get('graphics');
  const other = get('other');
  const native = get('native');
  const java = get('java');
  const stack = get('stack');
  // The "other" category mixes anonymous and non-anonymous regions; its
  // anonymous part (plus swap, which is always anonymous) belongs in the
  // Anonymous block, the remainder in Other.
  const otherAnon = other ? other.anon + other.swap : 0;
  const otherNonAnon = other ? Math.max(0, other.rss - other.anon) : 0;
  const anonComps = comps([native, java, stack]);
  const anonBytes =
    resSwap(native) + resSwap(java) + resSwap(stack) + otherAnon;

  const row1: MemBlock[] = [
    {
      label: 'File-backed',
      bytes: resSwap(file),
      color: smapsCategoryColor('file'),
      ...comps([file]),
    },
    {
      label: 'Anonymous',
      bytes: anonBytes,
      color: smapsCategoryColor('native'),
      // The "other anon" share has no private/shared split; anonymous
      // resident memory is almost always private dirty, so count it there.
      dirtySwap: anonComps.dirtySwap + otherAnon,
      clean: anonComps.clean,
      shared: anonComps.shared,
    },
    {
      label: 'Graphics',
      bytes: resSwap(graphics),
      color: smapsCategoryColor('graphics'),
      ...comps([graphics]),
    },
    {label: 'Other', bytes: otherNonAnon, color: smapsCategoryColor('other')},
  ];
  const row2: MemBlock[] = [
    {label: '', bytes: resSwap(file)}, // spacer under File-backed
    {
      label: 'Native',
      bytes: resSwap(native),
      color: smapsCategoryColor('native'),
      ...comps([native]),
    },
    {
      label: 'Java',
      bytes: resSwap(java),
      color: smapsCategoryColor('java'),
      ...comps([java]),
    },
    {
      label: 'Thread stacks',
      bytes: resSwap(stack),
      color: smapsCategoryColor('stack'),
      ...comps([stack]),
    },
    {label: 'Other anon', bytes: otherAnon, color: smapsCategoryColor('other')},
  ];
  return {row1, row2};
}

// Does all the per-process data-prep up front: filter to the selected
// process and build the memory-map snapshot/chart inputs. Pure function of
// (trace, data, process); the result is cached by the caller and reused
// across redraws.
function computeViewModel(
  trace: Trace,
  data: Data,
  processName?: string,
): ViewModel {
  const filteredData: Data = {
    heapDumps: data.heapDumps.filter((r) => r.processName === processName),
    heapProfiles: data.heapProfiles.filter(
      (r) => r.processName === processName,
    ),
    javaHeapCounters: data.javaHeapCounters.filter(
      (r) => r.processName === processName,
    ),
    smapsSummaries: data.smapsSummaries.filter(
      (r) => r.processName === processName,
    ),
    smapsCategories: data.smapsCategories.filter(
      (r) => r.processName === processName,
    ),
    heapStats: data.heapStats.filter((r) => r.processName === processName),
    heapClasses: data.heapClasses.filter((r) => r.processName === processName),
    bitmaps: data.bitmaps.filter((r) => r.processName === processName),
    nativeStacks: data.nativeStacks.filter(
      (r) => r.processName === processName,
    ),
    counterMaxima: data.counterMaxima.filter(
      (r) => r.processName === processName,
    ),
    threadCounts: data.threadCounts.filter(
      (r) => r.processName === processName,
    ),
    smapsPaths: data.smapsPaths.filter((r) => r.processName === processName),
  };

  const smapsSnapshots = buildSmapsSnapshots(filteredData.smapsCategories);
  const compChartData = buildCompositionChartData(trace, smapsSnapshots);

  return {
    filteredData,
    smapsSnapshots,
    compChartData,
  };
}

export class MemscopeLandingPage implements m.ClassComponent<{trace: Trace}> {
  private readonly dataSlot = new QuerySlot<Data>();
  private selectedProcessName?: string;
  // Memory map selection: `sel` is the snapshot shown in the block breakdown,
  // `base` (when set, via brushing) is an earlier snapshot to diff against.
  private memmapSel?: {sel: number; base?: number};
  // Label of the memory-map block under the cursor (drives the explainer).
  private memmapHoverLabel?: string;
  // Active top-level tab.
  private activeTab: 'summary' | 'smaps' = 'summary';
  // Smaps detail tab view state.
  private smapsFlat = false;
  private smapsAllCols = false;
  private smapsFilter = '';
  // Collapsed group/subgroup keys in the smaps detail tree.
  private readonly smapsCollapsed = new Set<string>();

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
                  this.memmapSel = undefined;
                },
              },
              processes.map((p) =>
                m('option', {value: p.name}, processOptionLabel(p)),
              ),
            ),
          ]),
        this.renderCaptureStrip(trace, vm.filteredData),
        this.renderTabs(),
        this.activeTab === 'summary'
          ? this.renderSections(trace, vm)
          : this.renderSmapsDetail(vm),
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

  // One score card holding 1–2 stat groups (label above, value, optional
  // sub-line). Used for the top billboard row and the per-section cards.
  private statCard(
    stats: {label: string; value: m.Children; sub?: m.Children}[],
  ): m.Child {
    return m(
      '.pf-memscope-billboard',
      stats.map((s) =>
        m('.pf-memscope-billboard__stat', [
          m('.pf-memscope-billboard__label', s.label),
          m('.pf-memscope-billboard__value', s.value),
          s.sub !== undefined && m('.pf-memscope-billboard__sub', s.sub),
        ]),
      ),
    );
  }

  // Tiny horizontal progress bar used for the "share %" table columns.
  private shareBar(frac: number): m.Child {
    const pct = Math.max(0, Math.min(100, frac * 100));
    return m('.pf-memscope-sharebar', [
      m('.pf-memscope-sharebar__fill', {style: {width: `${pct}%`}}),
      m('span.pf-memscope-sharebar__pct', `${Math.round(pct)}%`),
    ]);
  }

  // Callout strip: lightbulb insight (default) or amber warning.
  private callout(kind: 'insight' | 'warning', children: m.Children): m.Child {
    return m(
      '.pf-memscope-callout',
      {
        className:
          kind === 'warning' ? 'pf-memscope-callout--warning' : undefined,
      },
      m(Icon, {icon: kind === 'warning' ? 'info' : 'lightbulb'}),
      m('span', children),
    );
  }

  // Single-ratio bar (heap reachability, profiler coverage): an uppercase
  // label, a big percentage headline, a two-tone bar and a two-entry legend.
  private ratioBar(opts: {
    label: string;
    tooltip: string;
    pct: number;
    headline: string;
    color: string;
    aLabel: string;
    aBytes: number;
    bLabel: string;
    bBytes: number;
  }): m.Child {
    const pct = Math.max(0, Math.min(100, opts.pct));
    return m('.pf-memscope-ratio', [
      m('.pf-memscope-ratio__label', {title: opts.tooltip}, opts.label),
      m('.pf-memscope-ratio__headline', [
        m('span.pf-memscope-ratio__pct', `${Math.round(pct)}%`),
        m('span.pf-memscope-ratio__text', opts.headline),
      ]),
      m(
        '.pf-memscope-ratio__bar',
        m('.pf-memscope-ratio__fill', {
          style: {width: `${pct}%`, background: opts.color},
        }),
      ),
      m('.pf-memscope-ratio__legend', [
        m('span.pf-memscope-growth__legend-item', [
          m('span.pf-memscope-growth__swatch', {
            style: {background: opts.color},
          }),
          m('span', opts.aLabel),
          m('span.pf-memscope-growth__legend-delta', formatBytes(opts.aBytes)),
        ]),
        m('span.pf-memscope-growth__legend-item', [
          m('span.pf-memscope-growth__swatch', {
            style: {background: '#c3c7cc'},
          }),
          m('span', opts.bLabel),
          m('span.pf-memscope-growth__legend-delta', formatBytes(opts.bBytes)),
        ]),
      ]),
    ]);
  }

  // Compact titled table used by the Java / bitmaps / native sections.
  private topTable(opts: {
    title: string;
    subtitle?: string;
    cols: {label: string; num?: boolean}[];
    rows: m.Child[][];
  }): m.Child {
    return m('.pf-memscope-toptable', [
      m('.pf-memscope-toptable__title', [
        opts.title,
        opts.subtitle !== undefined &&
          m('span.pf-memscope-toptable__subtitle', ` · ${opts.subtitle}`),
      ]),
      m(
        'table.pf-memscope-table.pf-memscope-table--flush',
        m(
          'thead',
          m(
            'tr',
            opts.cols.map((c) =>
              m(c.num ? 'th.pf-memscope-table__num' : 'th', c.label),
            ),
          ),
        ),
        m(
          'tbody',
          opts.rows.map((cells) =>
            m(
              'tr',
              cells.map((cell, i) =>
                m(opts.cols[i].num ? 'td.pf-memscope-table__num' : 'td', cell),
              ),
            ),
          ),
        ),
      ),
    ]);
  }

  private className(full: string): m.Child {
    return m('span.pf-memscope-classname', {title: full}, shortClassName(full));
  }

  // Top billboard row: uptime + OOM score, peak RSS + spike, memory Δ +
  // trend. Each pair shares one card; cards without data are dropped.
  private renderScoreCards(vm: ViewModel): m.Children {
    const data = vm.filteredData;
    const cards: m.Child[] = [];

    const lastStats = data.heapStats[data.heapStats.length - 1];
    if (lastStats !== undefined) {
      const stats: {label: string; value: m.Children; sub?: m.Children}[] = [];
      if (lastStats.processUptime !== undefined) {
        stats.push({
          label: 'Uptime',
          value: formatDurationShort(lastStats.processUptime),
        });
      }
      if (lastStats.oomScoreAdj !== undefined) {
        stats.push({
          label: 'OOM score',
          value: `${lastStats.oomScoreAdj}`,
          sub: m(
            'span.pf-memscope-oom-chip',
            oomScoreLabel(lastStats.oomScoreAdj),
          ),
        });
      }
      if (stats.length > 0) cards.push(this.statCard(stats));
    }

    if (data.smapsSummaries.length > 0) {
      const peak = Math.max(...data.smapsSummaries.map((r) => r.anonAndSwap));
      const stats: {label: string; value: m.Children; sub?: m.Children}[] = [
        {label: 'Peak RSS anon+swap', value: formatBytes(peak)},
      ];
      const wm = data.counterMaxima.find(
        (c) => c.counterName === 'mem.rss.watermark',
      )?.maxValue;
      const rssMax = data.counterMaxima.find(
        (c) => c.counterName === 'mem.rss',
      )?.maxValue;
      if (wm !== undefined && rssMax !== undefined && wm > rssMax) {
        stats.push({
          label: 'RSS spike',
          value: formatDelta(wm - rssMax),
          sub: 'hi-watermark − max',
        });
      }
      cards.push(this.statCard(stats));
    }

    const rows = data.smapsSummaries;
    if (rows.length > 1) {
      const first = rows[0];
      const last = rows[rows.length - 1];
      const total = last.anonAndSwap - first.anonAndSwap;
      const spanSeconds = Number(last.ts - first.ts) / 1e9;
      const stats: {label: string; value: m.Children; sub?: m.Children}[] = [
        {label: 'Memory Δ', value: formatDelta(total), sub: 'first → last'},
      ];
      if (spanSeconds > 0) {
        stats.push({
          label: 'Trend',
          value: `${formatDelta((total / spanSeconds) * 3600)}/h`,
        });
      }
      cards.push(this.statCard(stats));
    }

    if (cards.length === 0) return null;
    return m('.pf-memscope-billboards', cards);
  }

  private renderTabs(): m.Children {
    const tabs: {key: 'summary' | 'smaps'; label: string; icon: string}[] = [
      {key: 'summary', label: 'Summary', icon: 'description'},
      {key: 'smaps', label: 'Smaps detail', icon: 'table_rows'},
    ];
    return m(
      '.pf-memscope-tabs',
      tabs.map((t) =>
        m(
          'button.pf-memscope-tab',
          {
            className:
              this.activeTab === t.key ? 'pf-memscope-tab--active' : undefined,
            onclick: () => (this.activeTab = t.key),
          },
          [m(Icon, {icon: t.icon}), t.label],
        ),
      ),
    );
  }

  // "How much Java memory did you use, and where did it go?" — from the full
  // heap graph: reachability, totals, and the three top-classes tables.
  private renderJavaSection(trace: Trace, vm: ViewModel): m.Children {
    const data = vm.filteredData;
    if (data.heapStats.length === 0) return null;
    const last = data.heapStats[data.heapStats.length - 1];
    const classes = data.heapClasses;

    const reachableTotal = last.reachableHeapSize + last.reachableNativeSize;
    const unreachableHeap = last.totalHeapSize - last.reachableHeapSize;
    const reachPct =
      last.totalHeapSize > 0
        ? (last.reachableHeapSize / last.totalHeapSize) * 100
        : 0;

    // ART overhead from the smaps paths of the last snapshot.
    const artBytes = data.smapsPaths
      .filter((p) => /\.art|\.oat|\.odex|\.vdex|dalvik-jit/.test(p.path))
      .reduce((sum, p) => sum + p.rss, 0);

    // Insight: the class retaining the most (reachable self + native).
    const topRetainer = classes
      .slice()
      .sort((a, b) => a.rnRetained - b.rnRetained)[0];
    const insight: m.Children = [];
    if (topRetainer !== undefined && reachableTotal > 0) {
      const retained =
        topRetainer.reachableSizeBytes + topRetainer.reachableNativeSizeBytes;
      insight.push(
        m('b', shortClassName(topRetainer.typeName)),
        ' retains ',
        m('b', formatBytes(retained)),
        ' — ',
        m(
          'b',
          `${Math.round((retained / reachableTotal) * 100)}% of the ` +
            'Java heap',
        ),
        ' once native buffers are counted.',
      );
    }
    if (last.totalHeapSize > 0) {
      insight.push(
        ' ',
        m('b', `${Math.round(100 - reachPct)}%`),
        ' of the heap is currently unreachable (awaiting GC).',
      );
    }

    const retainerRows = classes
      .filter((c) => c.rnRetained <= 5)
      .sort((a, b) => a.rnRetained - b.rnRetained)
      .map((c) => [
        this.className(c.typeName),
        formatBytes(c.reachableSizeBytes),
        c.reachableNativeSizeBytes > 0
          ? formatDelta(c.reachableNativeSizeBytes)
          : '—',
        c.reachableObjCount.toLocaleString(),
        this.shareBar(
          reachableTotal > 0
            ? (c.reachableSizeBytes + c.reachableNativeSizeBytes) /
                reachableTotal
            : 0,
        ),
      ]);

    const dominatorRows = classes
      .filter((c) => c.rnDominated <= 5)
      .sort((a, b) => a.rnDominated - b.rnDominated)
      .map((c) => [
        this.className(c.typeName),
        formatBytes(c.dominatedSizeBytes + c.dominatedNativeSizeBytes),
        c.dominatedObjCount.toLocaleString(),
        this.shareBar(
          reachableTotal > 0
            ? (c.dominatedSizeBytes + c.dominatedNativeSizeBytes) /
                reachableTotal
            : 0,
        ),
      ]);

    const countRows = classes
      .filter((c) => c.rnCount <= 5)
      .sort((a, b) => a.rnCount - b.rnCount)
      .map((c) => [
        this.className(c.typeName),
        c.objCount.toLocaleString(),
        formatBytes(c.sizeBytes),
        formatBytes(c.dominatedSizeBytes + c.dominatedNativeSizeBytes),
        this.shareBar(
          last.totalObjCount > 0 ? c.objCount / last.totalObjCount : 0,
        ),
      ]);

    return m(
      Panel,
      {
        title: 'How much Java memory did you use, and where did it go?',
        subtitle:
          'From the full heap graph — complete and exact, with retention ' +
          'but no callstacks.',
        controls: m(
          'a.pf-memscope-memmap__link',
          {
            onclick: () => {
              trace.navigate(`#!/heapdump?upid=${last.upid}&ts=${last.ts}`);
            },
          },
          [m(Icon, {icon: 'memory'}), 'Open heap dump explorer'],
        ),
      },
      m('.pf-memscope-section', [
        insight.length > 0 && this.callout('insight', insight),
        this.ratioBar({
          label: 'Heap reachability',
          tooltip:
            'Reachable = objects a GC root still references. Unreachable ' +
            'objects are garbage awaiting collection.',
          pct: reachPct,
          headline: 'of the Java heap is reachable',
          color: '#f4b400',
          aLabel: 'Reachable',
          aBytes: last.reachableHeapSize,
          bLabel: 'Unreachable',
          bBytes: unreachableHeap,
        }),
        m('.pf-memscope-billboards.pf-memscope-billboards--section', [
          this.statCard([
            {
              label: 'Heap',
              value: formatBytes(last.totalHeapSize),
              sub: 'reachable + unreachable',
            },
          ]),
          this.statCard([
            {
              label: 'Live objects',
              value: last.totalObjCount.toLocaleString(),
              sub:
                `${last.reachableObjCount.toLocaleString()} reach · ` +
                `${(
                  last.totalObjCount - last.reachableObjCount
                ).toLocaleString()} unreach`,
            },
          ]),
          this.statCard([
            {
              label: 'Registered native',
              value: formatBytes(last.reachableNativeSize),
              sub: 'owned by Java (bitmaps, NIO)',
            },
          ]),
          artBytes > 0 &&
            this.statCard([
              {
                label: 'ART overhead',
                value: formatBytes(artBytes),
                sub: '.art images + JIT cache',
              },
            ]),
        ]),
        m('.pf-memscope-tables', [
          retainerRows.length > 0 &&
            this.topTable({
              title: 'Top retainers by class',
              cols: [
                {label: 'Class'},
                {label: 'Retained', num: true},
                {label: '+native', num: true},
                {label: 'Inst.', num: true},
                {label: 'Share', num: true},
              ],
              rows: retainerRows,
            }),
          dominatorRows.length > 0 &&
            this.topTable({
              title: 'Top dominators',
              cols: [
                {label: 'Class'},
                {label: 'Retained', num: true},
                {label: 'R.count', num: true},
                {label: 'Share', num: true},
              ],
              rows: dominatorRows,
            }),
          countRows.length > 0 &&
            this.topTable({
              title: 'Top classes by instance count',
              cols: [
                {label: 'Class'},
                {label: 'Instances', num: true},
                {label: 'Shallow', num: true},
                {label: 'Retained', num: true},
                {label: 'Share of count', num: true},
              ],
              rows: countRows,
            }),
        ]),
      ]),
    );
  }

  // "What about bitmaps?" — reachable android.graphics.Bitmap instances
  // grouped by dimensions and storage backing.
  private renderBitmapsSection(vm: ViewModel): m.Children {
    const data = vm.filteredData;
    const groups = data.bitmaps;
    if (groups.length === 0) return null;

    // Pixel bytes for non-heap backings (ashmem / hardware) are not part of
    // self/native size — estimate them as w*h*4 when dimensions are known.
    const estPixelBytes = (g: BitmapGroupRow) =>
      g.storage !== 'heap' && g.width !== undefined && g.height !== undefined
        ? g.width * g.height * 4 * g.count
        : 0;
    const bytesOf = (g: BitmapGroupRow) =>
      g.selfSize + g.nativeSize + estPixelBytes(g);

    const totalCount = groups.reduce((s, g) => s + g.count, 0);
    const totalBytes = groups.reduce((s, g) => s + bytesOf(g), 0);
    const heapBytes = groups
      .filter((g) => g.storage === 'heap')
      .reduce((s, g) => s + g.selfSize + g.nativeSize, 0);
    const ashmemBytes = groups
      .filter((g) => g.storage === 'ashmem')
      .reduce((s, g) => s + bytesOf(g), 0);

    // Re-group by dimensions only.
    const byDims = new Map<string, {count: number; bytes: number}>();
    for (const g of groups) {
      const key =
        g.width !== undefined && g.height !== undefined
          ? `${g.width}×${g.height}`
          : 'unknown';
      const e = byDims.get(key) ?? {count: 0, bytes: 0};
      e.count += g.count;
      e.bytes += bytesOf(g);
      byDims.set(key, e);
    }
    const dims = Array.from(byDims.entries()).map(([key, e]) => ({
      key,
      ...e,
    }));
    const bySize = dims.slice().sort((a, b) => b.bytes - a.bytes);
    const byCount = dims.slice().sort((a, b) => b.count - a.count);
    const hasDims = dims.some((d) => d.key !== 'unknown');
    const largest = bySize[0];

    const lastStats = data.heapStats[data.heapStats.length - 1];
    const javaRetained =
      lastStats !== undefined
        ? lastStats.reachableHeapSize + lastStats.reachableNativeSize
        : 0;

    const insight: m.Children = hasDims
      ? [
          m('b', largest.count.toLocaleString()),
          ' bitmaps of ',
          m('b', largest.key),
          ' alone account for ',
          m('b', formatBytes(largest.bytes)),
          ashmemBytes > heapBytes
            ? [
                '. Most bitmap bytes live in ',
                m('b', 'ashmem'),
                ` (${formatBytes(ashmemBytes)}) rather than the Java heap ` +
                  `(${formatBytes(heapBytes)}).`,
              ]
            : '.',
        ]
      : [
          'This trace format does not record bitmap dimensions — only ' +
            'counts and sizes are available.',
        ];

    const dimRows = (list: {key: string; count: number; bytes: number}[]) =>
      list
        .slice(0, 5)
        .map((d) => [
          d.key,
          d.count.toLocaleString(),
          formatBytes(d.bytes),
          this.shareBar(totalBytes > 0 ? d.bytes / totalBytes : 0),
        ]);

    return m(
      Panel,
      {
        title: 'What about bitmaps?',
        subtitle:
          'Usually the largest and most reducible cost on Android — pulled ' +
          'out on their own.',
      },
      m('.pf-memscope-section', [
        this.callout('insight', insight),
        m('.pf-memscope-billboards.pf-memscope-billboards--section', [
          this.statCard([
            {
              label: 'Total bitmaps',
              value: totalCount.toLocaleString(),
              sub: 'live android.graphics.Bitmap',
            },
          ]),
          this.statCard([
            {
              label: 'Bitmap memory',
              value: formatBytes(totalBytes),
              sub: `${formatBytes(heapBytes)} heap · ${formatBytes(
                ashmemBytes,
              )} ashmem (est.)`,
            },
          ]),
          hasDims &&
            this.statCard([
              {
                label: 'Largest group',
                value: formatBytes(largest.bytes),
                sub: `${largest.key} ×${largest.count}`,
              },
            ]),
          javaRetained > 0 &&
            this.statCard([
              {
                label: 'Of Java retained',
                value: `${Math.round(
                  (groups.reduce((s, g) => s + g.selfSize + g.nativeSize, 0) /
                    javaRetained) *
                    100,
                )}%`,
                sub: 'share of heap + native',
              },
            ]),
        ]),
        hasDims &&
          m('.pf-memscope-tables', [
            this.topTable({
              title: 'Largest bitmaps',
              subtitle: 'grouped by dimensions',
              cols: [
                {label: 'Dimensions'},
                {label: 'Count', num: true},
                {label: 'Size', num: true},
                {label: 'Share', num: true},
              ],
              rows: dimRows(bySize),
            }),
            this.topTable({
              title: 'Most frequent bitmaps',
              subtitle: 'grouped by dimensions',
              cols: [
                {label: 'Dimensions'},
                {label: 'Count', num: true},
                {label: 'Size', num: true},
                {label: 'Share', num: true},
              ],
              rows: dimRows(byCount),
            }),
          ]),
      ]),
    );
  }

  // "How much native memory did you use, and where did it go?" — heapprofd
  // coverage vs the smaps native total, plus the top unreleased callstacks.
  private renderNativeSection(vm: ViewModel): m.Children {
    const data = vm.filteredData;
    if (data.heapProfiles.length === 0) return null;

    // Unreleased bytes at the last sample of each heap.
    const lastByHeap = new Map<string, HeapProfileRow>();
    for (const r of data.heapProfiles) lastByHeap.set(r.heapName, r);
    let seen = 0;
    for (const r of lastByHeap.values()) seen += r.unreleasedSize;

    // Native allocator footprint from the last smaps snapshot.
    const lastSnap = vm.smapsSnapshots[vm.smapsSnapshots.length - 1];
    const nativeRow = lastSnap?.byCategory.get('native');
    const nativeRss =
      nativeRow !== undefined ? nativeRow.rss + nativeRow.swap : 0;
    const stackRow = lastSnap?.byCategory.get('stack');
    const stackBytes =
      stackRow !== undefined ? stackRow.rss + stackRow.swap : 0;
    const threads = data.threadCounts[0]?.count;
    const coveragePct = nativeRss > 0 ? (seen / nativeRss) * 100 : undefined;
    const overhead = nativeRss > seen ? nativeRss - seen : 0;

    const stackRows = data.nativeStacks.slice(0, 5).map((s) => [
      m(
        '.pf-memscope-stack',
        stackSnippet(s.frames).map((f, i) => [
          i > 0 && m('span.pf-memscope-stack__sep', ' ← '),
          m('span', {title: f}, f.length > 48 ? `${f.slice(0, 46)}…` : f),
        ]),
      ),
      formatBytes(s.unreleased),
      s.allocs.toLocaleString(),
      this.shareBar(seen > 0 ? s.unreleased / seen : 0),
    ]);

    return m(
      Panel,
      {
        title: 'How much native memory did you use, and where did it go?',
        subtitle:
          'Aggregated by allocation callstack — the clearest signal when ' +
          "it's available.",
      },
      m('.pf-memscope-section', [
        this.callout('warning', [
          'The native profiler only sees allocations made ',
          m('b', 'after'),
          " tracing started — it can't explain the past. Leaks present " +
            'before t=0 are invisible here; trust the composition totals ' +
            'above for the full resident picture.',
        ]),
        coveragePct !== undefined &&
          this.ratioBar({
            label: 'Profiler coverage',
            tooltip:
              'Unreleased bytes the profiler observed vs the native ' +
              'allocator footprint from smaps. The rest predates the trace.',
            pct: coveragePct,
            headline: 'of native RSS explained by the profiler',
            color: '#4285f4',
            aLabel: 'Seen by profiler',
            aBytes: seen,
            bLabel: 'Unseen (pre-trace)',
            bBytes: overhead,
          }),
        m('.pf-memscope-billboards.pf-memscope-billboards--section', [
          nativeRss > 0 &&
            this.statCard([
              {
                label: 'RSS anon + swap',
                value: formatBytes(nativeRss),
                sub: 'total native private footprint',
              },
            ]),
          this.statCard([
            {
              label: 'Seen by profiler',
              value: formatBytes(seen),
              sub:
                coveragePct !== undefined
                  ? `${Math.round(coveragePct)}% coverage`
                  : 'unreleased at last sample',
            },
          ]),
          overhead > 0 &&
            this.statCard([
              {
                label: 'Allocator overhead + unseen',
                value: formatBytes(overhead),
                sub: 'metadata, fragmentation, pre-trace',
              },
            ]),
          stackBytes > 0 &&
            this.statCard([
              {
                label: 'Thread stacks',
                value: formatBytes(stackBytes),
                sub: threads !== undefined ? `${threads} threads` : undefined,
              },
            ]),
        ]),
        stackRows.length > 0 &&
          this.topTable({
            title: 'Top allocation call-stacks',
            subtitle: 'unreleased at last snapshot · boring frames elided',
            cols: [
              {label: 'Call-stack snippet'},
              {label: 'Unreleased', num: true},
              {label: 'Allocs', num: true},
              {label: 'Share of profiled', num: true},
            ],
            rows: stackRows,
          }),
      ]),
    );
  }

  // "Smaps detail" tab: every mapping of the last snapshot, folded into the
  // two-level SMAPS_TREE taxonomy (or flat), with regex filtering, optional
  // extra columns, and collapsible groups.
  private renderSmapsDetail(vm: ViewModel): m.Children {
    const rows = vm.filteredData.smapsPaths;
    if (rows.length === 0) {
      return m(
        '.pf-memscope-landing__empty',
        'No smaps data found for this process.',
      );
    }

    const sum = (list: SmapsPathRow[], get: (r: SmapsPathRow) => number) =>
      list.reduce((s, r) => s + get(r), 0);

    let filtered = rows;
    if (this.smapsFilter !== '') {
      try {
        const re = new RegExp(this.smapsFilter);
        filtered = rows.filter((r) => re.test(r.path));
      } catch {
        filtered = rows.filter((r) => r.path.includes(this.smapsFilter));
      }
    }

    const cols: {label: string; get: (r: SmapsPathRow) => number}[] = this
      .smapsAllCols
      ? [
          {label: 'RSS', get: (r) => r.rss},
          {label: 'PSS', get: (r) => r.pss},
          {label: 'Anon+swap', get: (r) => r.anon + r.swap},
          {label: 'Priv. dirty', get: (r) => r.privateDirty},
          {label: 'Priv. clean', get: (r) => r.privateClean},
          {label: 'Shared dirty', get: (r) => r.sharedDirty},
          {label: 'Shared clean', get: (r) => r.sharedClean},
          {label: 'Swap', get: (r) => r.swap},
        ]
      : [
          {label: 'RSS', get: (r) => r.rss},
          {label: 'Priv. dirty', get: (r) => r.privateDirty},
          {label: 'Swap', get: (r) => r.swap},
        ];

    const fmtCell = (n: number) => (n > 0 ? formatBytes(n) : '—');
    const numCells = (r: SmapsPathRow) =>
      cols.map((c) => m('td.pf-memscope-table__num', fmtCell(c.get(r))));
    const sumCells = (list: SmapsPathRow[]) =>
      cols.map((c) =>
        m(
          'td.pf-memscope-table__num.pf-memscope-table__size',
          fmtCell(sum(list, c.get)),
        ),
      );

    const toggle = (key: string) => {
      if (this.smapsCollapsed.has(key)) {
        this.smapsCollapsed.delete(key);
      } else {
        this.smapsCollapsed.add(key);
      }
    };
    const chevron = (key: string) =>
      m(Icon, {
        className: 'pf-memscope-smaps__chevron',
        icon: this.smapsCollapsed.has(key) ? 'chevron_right' : 'expand_more',
      });
    const swatch = (color: string) =>
      m('span.pf-memscope-growth__swatch', {style: {background: color}});

    const MAX_CHILD_ROWS = 30;
    const pathRow = (r: SmapsPathRow, indent: string) =>
      m('tr', [m(`td.pf-memscope-smaps__path.${indent}`, r.path), numCells(r)]);
    const moreRow = (n: number, indent: string) =>
      m(
        'tr.pf-memscope-smaps__more',
        m(
          `td.${indent}`,
          {colspan: cols.length + 1},
          `… ${n} more mappings (filter to narrow down)`,
        ),
      );

    let body: m.Child[];
    if (this.smapsFlat) {
      body = filtered
        .slice(0, 200)
        .map((r) => pathRow(r, 'pf-memscope-smaps__lvl0'));
      if (filtered.length > 200) {
        body.push(moreRow(filtered.length - 200, 'pf-memscope-smaps__lvl0'));
      }
    } else {
      // Bucket each mapping into its (group, sub) of the taxonomy.
      const buckets = new Map<string, SmapsPathRow[]>();
      for (const r of filtered) {
        const {group, sub} = classifyMapping(r.path);
        const key = `${group}/${sub}`;
        const list = buckets.get(key) ?? [];
        list.push(r);
        buckets.set(key, list);
      }

      body = [];
      for (const g of SMAPS_TREE) {
        const subLists = g.subs
          .map((s) => ({
            meta: s,
            rows: buckets.get(`${g.key}/${s.key}`) ?? [],
          }))
          .filter((s) => s.rows.length > 0)
          .sort(
            (a, b) => sum(b.rows, (r) => r.rss) - sum(a.rows, (r) => r.rss),
          );
        if (subLists.length === 0) continue;
        const groupRows = subLists.flatMap((s) => s.rows);

        body.push(
          m('tr.pf-memscope-smaps__group', {onclick: () => toggle(g.key)}, [
            m('td', [chevron(g.key), swatch(g.color), ` ${g.label}`]),
            sumCells(groupRows),
          ]),
        );
        if (this.smapsCollapsed.has(g.key)) continue;

        for (const s of subLists) {
          const subKey = `${g.key}/${s.meta.key}`;
          body.push(
            m(
              'tr.pf-memscope-smaps__subgroup',
              {onclick: () => toggle(subKey)},
              [
                m('td.pf-memscope-smaps__lvl1', [
                  chevron(subKey),
                  swatch(s.meta.color),
                  ` ${s.meta.label} `,
                  m('span.pf-memscope-smaps__count', `· ${s.rows.length} maps`),
                ]),
                sumCells(s.rows),
              ],
            ),
          );
          if (this.smapsCollapsed.has(subKey)) continue;
          const sorted = s.rows.slice().sort((a, b) => b.rss - a.rss);
          for (const r of sorted.slice(0, MAX_CHILD_ROWS)) {
            body.push(pathRow(r, 'pf-memscope-smaps__lvl2'));
          }
          if (sorted.length > MAX_CHILD_ROWS) {
            body.push(
              moreRow(
                sorted.length - MAX_CHILD_ROWS,
                'pf-memscope-smaps__lvl2',
              ),
            );
          }
        }
      }
    }

    return m(
      Panel,
      {
        title: 'Every mapping, grouped',
        subtitle:
          'The raw /proc/<pid>/smaps dump, folded into the same taxonomy ' +
          'as the composition above. Filter by regex, or flatten to see ' +
          'individual mappings.',
      },
      m('.pf-memscope-section', [
        m('.pf-memscope-billboards.pf-memscope-billboards--section', [
          this.statCard([
            {
              label: 'Total RSS',
              value: formatBytes(sum(rows, (r) => r.rss)),
              sub: `${rows.length} mappings`,
            },
          ]),
          this.statCard([
            {
              label: 'RSS anon + swap',
              value: formatBytes(sum(rows, (r) => r.anon + r.swap)),
              sub: 'private anon + swapped',
            },
          ]),
          this.statCard([
            {
              label: 'Total PSS',
              value: formatBytes(sum(rows, (r) => r.pss)),
              sub: 'proportional set size',
            },
          ]),
          this.statCard([
            {
              label: 'Private dirty',
              value: formatBytes(sum(rows, (r) => r.privateDirty)),
              sub: 'unshareable cost',
            },
          ]),
          this.statCard([
            {
              label: 'Swap',
              value: formatBytes(sum(rows, (r) => r.swap)),
              sub: 'zram / swapped out',
            },
          ]),
        ]),
        m('.pf-memscope-smaps__controls', [
          m(
            'button.pf-memscope-tab',
            {
              className: !this.smapsFlat
                ? 'pf-memscope-tab--active'
                : undefined,
              onclick: () => (this.smapsFlat = false),
            },
            [m(Icon, {icon: 'account_tree'}), 'Tree'],
          ),
          m(
            'button.pf-memscope-tab',
            {
              className: this.smapsFlat ? 'pf-memscope-tab--active' : undefined,
              onclick: () => (this.smapsFlat = true),
            },
            [m(Icon, {icon: 'format_list_bulleted'}), 'Flat'],
          ),
          m('input.pf-memscope-smaps__filter', {
            type: 'text',
            placeholder: 'Filter by regex, e.g. \\.so$ or dalvik|scudo',
            value: this.smapsFilter,
            oninput: (e: Event) => {
              this.smapsFilter = (e.target as HTMLInputElement).value;
            },
          }),
          m(
            'button.pf-memscope-tab',
            {
              className: this.smapsAllCols
                ? 'pf-memscope-tab--active'
                : undefined,
              onclick: () => (this.smapsAllCols = !this.smapsAllCols),
            },
            [m(Icon, {icon: 'view_column'}), 'All columns'],
          ),
          m(
            'span.pf-memscope-smaps__counttext',
            `${filtered.length} / ${rows.length} mappings`,
          ),
        ]),
        m(
          'table.pf-memscope-table.pf-memscope-table--flush',
          m(
            'thead',
            m('tr', [
              m('th', 'Region / path'),
              cols.map((c) => m('th.pf-memscope-table__num', c.label)),
            ]),
          ),
          m('tbody', body),
        ),
      ]),
    );
  }

  // "Where did all the memory go?" panel: smaps-only memory map. An insight
  // callout, a stacked composition-over-time chart with snapshot selection
  // (chips to pick one, brush to compare two), and a two-level block
  // breakdown of the selected snapshot shaded by dirty/clean/shared.
  private renderMemoryMap(trace: Trace, vm: ViewModel): m.Children {
    const snaps = vm.smapsSnapshots;
    if (snaps.length === 0 || vm.compChartData === undefined) return null;

    const xOf = (s: SmapsSnapshot) =>
      Number(s.ts - trace.traceInfo.start) / 1e9;
    const lastIdx = snaps.length - 1;
    const selIdx = Math.min(this.memmapSel?.sel ?? lastIdx, lastIdx);
    let baseIdx =
      this.memmapSel?.base !== undefined
        ? Math.min(this.memmapSel.base, lastIdx)
        : undefined;
    if (baseIdx === selIdx) baseIdx = undefined;
    const snap = snaps[selIdx];
    const base = baseIdx !== undefined ? snaps[baseIdx] : undefined;

    // Insight: biggest slice of the latest snapshot + fastest-growing slice.
    const catVal = (s: SmapsSnapshot, key: string) => {
      const r = s.byCategory.get(key);
      return r ? r.rss + r.swap : 0;
    };
    const first = snaps[0];
    const last = snaps[lastIdx];
    const stats = SMAPS_CATEGORIES.map((c) => ({
      ...c,
      last: catVal(last, c.key),
      delta: catVal(last, c.key) - catVal(first, c.key),
    }));
    const biggest = stats.reduce((a, b) => (b.last > a.last ? b : a));
    const fastest = stats.reduce((a, b) => (b.delta > a.delta ? b : a));
    const pct =
      last.total > 0 ? Math.round((biggest.last / last.total) * 100) : 0;
    const insight: m.Children = [
      m('b', `${biggest.label} memory`),
      ' is ',
      m('b', `${pct}% of the footprint`),
    ];
    if (snaps.length > 1 && fastest.delta > 0) {
      if (fastest.key === biggest.key) {
        insight.push(
          ' and the fastest-growing slice (',
          m('b', formatDelta(fastest.delta)),
          ').',
        );
      } else {
        insight.push(
          '; ',
          m('b', fastest.label),
          ' is the fastest-growing slice (',
          m('b', formatDelta(fastest.delta)),
          ').',
        );
      }
    } else {
      insight.push('.');
    }

    // Snapshot block breakdown, optionally diffed against `base`.
    const blocks = buildMemBlocks(snap);
    const baseBlocks = base !== undefined ? buildMemBlocks(base) : undefined;
    const renderRow = (row: MemBlock[], baseRow?: MemBlock[]) =>
      m(
        '.pf-memscope-memmap__row',
        row.map((b, i) => {
          const widthPct = snap.total > 0 ? (b.bytes / snap.total) * 100 : 0;
          if (b.color === undefined) {
            // Spacer: keeps row 2 aligned under the Anonymous block.
            return m('.pf-memscope-memmap__gap', {
              style: {width: `${widthPct}%`},
            });
          }
          if (b.bytes <= 0) return null;
          let background = b.color;
          if (b.dirtySwap !== undefined) {
            const total = b.dirtySwap + (b.clean ?? 0) + (b.shared ?? 0);
            if (total > 0) {
              const d = ((b.dirtySwap / total) * 100).toFixed(1);
              const dc = (
                ((b.dirtySwap + (b.clean ?? 0)) / total) *
                100
              ).toFixed(1);
              background =
                `linear-gradient(to right, ${b.color} 0% ${d}%, ` +
                `${b.color}88 ${d}% ${dc}%, #c3c7cc ${dc}% 100%)`;
            }
          }
          const delta =
            baseRow !== undefined ? b.bytes - baseRow[i].bytes : undefined;
          const tooltip = [`${b.label}: ${formatBytes(b.bytes)}`];
          if (b.dirtySwap !== undefined) {
            tooltip.push(`Dirty + swap: ${formatBytes(b.dirtySwap)}`);
            tooltip.push(`Clean: ${formatBytes(b.clean ?? 0)}`);
            tooltip.push(`Shared: ${formatBytes(b.shared ?? 0)}`);
          }
          if (delta !== undefined && baseIdx !== undefined) {
            tooltip.push(
              `Δ vs snapshot #${baseIdx + 1}: ${formatDelta(delta)}`,
            );
          }
          return m(
            '.pf-memscope-memmap__block',
            {
              style: {width: `${widthPct}%`, background},
              title: tooltip.join('\n'),
              onmouseenter: () => (this.memmapHoverLabel = b.label),
              onmouseleave: () => {
                if (this.memmapHoverLabel === b.label) {
                  this.memmapHoverLabel = undefined;
                }
              },
            },
            m('span.pf-memscope-memmap__block-label', b.label),
            m(
              'span.pf-memscope-memmap__block-value',
              delta !== undefined ? formatDelta(delta) : formatBytes(b.bytes),
            ),
          );
        }),
      );

    const shadeSwatch = (style: object) =>
      m('span.pf-memscope-memmap__shade-swatch', {style});

    const smapsTrackId = vm.filteredData.smapsSummaries.find(
      (r) => r.trackId !== undefined,
    )?.trackId;

    return m(
      Panel,
      {
        title: 'Where did all the memory go?',
        subtitle:
          'A complete breakdown of resident memory over time, from smaps. ' +
          'Click a snapshot to inspect it, or drag across the chart to ' +
          'compare two.',
        controls: m(
          'a.pf-memscope-memmap__link',
          {
            onclick: () => {
              trace.scrollTo({
                time: {start: snap.ts, behavior: 'pan'},
                track:
                  smapsTrackId !== undefined
                    ? {uri: `/counter_${smapsTrackId}`, expandGroup: true}
                    : undefined,
              });
              trace.navigate('#!/viewer');
            },
          },
          [m(Icon, {icon: 'timeline'}), 'Open timeline'],
        ),
      },
      m('.pf-memscope-memmap', [
        m('.pf-memscope-memmap__insight', [
          m(Icon, {icon: 'lightbulb'}),
          m('span', insight),
        ]),
        m('.pf-memscope-memmap__section-header', [
          m('span.pf-memscope-memmap__section-title', 'Composition over time'),
          m(
            'span.pf-memscope-memmap__section-hint',
            'click a snapshot · drag to compare',
          ),
          m('span.pf-memscope-memmap__badge', [
            m(Icon, {icon: 'photo_camera'}),
            `Snapshot #${selIdx + 1} · ${formatBytes(snap.total)}`,
          ]),
        ]),
        m(LineChartSvg, {
          data: vm.compChartData,
          height: 160,
          stacked: true,
          xAxisLabel: 'Time (s)',
          yAxisLabel: 'Size',
          showLegend: true,
          showPoints: true,
          gridLines: 'vertical',
          formatXValue: (v: number) => `${v.toFixed(0)}s`,
          formatYValue: (v: number) => formatBytes(v),
          markers: [{x: xOf(snap), label: `#${selIdx + 1}`}],
          selection:
            base !== undefined ? {start: xOf(base), end: xOf(snap)} : undefined,
          onBrush: ({start, end}: {start: number; end: number}) => {
            const nearest = (x: number) => {
              let bestIdx = 0;
              for (let i = 1; i < snaps.length; i++) {
                if (
                  Math.abs(xOf(snaps[i]) - x) <
                  Math.abs(xOf(snaps[bestIdx]) - x)
                ) {
                  bestIdx = i;
                }
              }
              return bestIdx;
            };
            const a = nearest(start);
            const b = nearest(end);
            this.memmapSel = a === b ? {sel: a} : {sel: b, base: a};
          },
        }),
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
                title: `t=${xOf(s).toFixed(1)}s · ${formatBytes(s.total)}`,
                onclick: () => (this.memmapSel = {sel: i}),
              },
              `#${i + 1}`,
            ),
          ),
        ),
        m('.pf-memscope-memmap__section-header', [
          m(
            'span.pf-memscope-memmap__section-title',
            baseIdx !== undefined
              ? `Snapshot #${baseIdx + 1} → #${selIdx + 1}`
              : `Snapshot #${selIdx + 1} — t=${xOf(snap).toFixed(0)}s`,
          ),
          m(
            'span.pf-memscope-memmap__section-hint',
            baseIdx !== undefined
              ? 'block sizes from the later snapshot · values are deltas'
              : 'absolute resident memory · hover a block',
          ),
          m('span.pf-memscope-memmap__shade-legend', [
            shadeSwatch({background: '#4285f488'}),
            'clean',
            shadeSwatch({background: '#4285f4'}),
            'dirty / swap',
            shadeSwatch({background: '#c3c7cc'}),
            'shared',
          ]),
        ]),
        m('.pf-memscope-memmap__rows', [
          renderRow(blocks.row1, baseBlocks?.row1),
          renderRow(blocks.row2, baseBlocks?.row2),
        ]),
        m(
          '.pf-memscope-memmap__footnote',
          this.memmapHoverLabel !== undefined &&
            MEMMAP_BLOCK_INFO[this.memmapHoverLabel] !== undefined
            ? [
                m('b', this.memmapHoverLabel),
                ` — ${MEMMAP_BLOCK_INFO[this.memmapHoverLabel]}`,
              ]
            : [
                'Hover any block to see what it represents. ',
                m('b', 'Darker'),
                ' = dirty or swapped (a private, unshareable cost); ',
                m('b', 'lighter'),
                ' = clean and reclaimable.',
              ],
        ),
      ]),
    );
  }

  // "Memory analysis" panel: attributes the anon+swap delta between the first
  // and last smaps snapshot to native heap / Java heap / other regions,
  // rendered as a single stacked bar. The bar shows the categories that moved
  // in the same direction as the total; the legend lists all of them.
  private renderGrowthCard(data: Data): m.Children {
    const rows = data.smapsSummaries;
    if (rows.length < 2) return null;
    const first = rows[0];
    const last = rows[rows.length - 1];
    const total = last.anonAndSwap - first.anonAndSwap;
    const categories = [
      {
        name: 'Native heap',
        color: '#4285f4',
        delta: last.nativeAnonAndSwap - first.nativeAnonAndSwap,
      },
      {
        name: 'Java heap',
        color: '#f4b400',
        delta: last.dalvikAnonAndSwap - first.dalvikAnonAndSwap,
      },
      {
        name: 'Other',
        color: '#9aa0a6',
        delta: last.otherAnonAndSwap - first.otherAnonAndSwap,
      },
    ].sort((a, b) => b.delta - a.delta);
    const shrinking = total < 0;
    // Growth rate, normalised to bytes/hour over the observed snapshot span.
    const spanSeconds = Number(last.ts - first.ts) / 1e9;
    const ratePerHour =
      spanSeconds > 0 ? (total / spanSeconds) * 3600 : undefined;
    const contributors = categories.filter((c) =>
      shrinking ? c.delta < 0 : c.delta > 0,
    );
    const contribTotal = contributors.reduce(
      (sum, c) => sum + Math.abs(c.delta),
      0,
    );

    return m(
      Panel,
      {
        title: 'Memory analysis',
        subtitle:
          'How anonymous + swap memory (from smaps) changed between the ' +
          'first and last snapshot, by region.',
      },
      m('.pf-memscope-growth', [
        m(
          '.pf-memscope-growth__label',
          shrinking ? 'Where the shrinkage went' : 'Where the growth went',
        ),
        m('.pf-memscope-growth__headline', [
          m('span.pf-memscope-growth__delta', formatDelta(total)),
          m(
            'span.pf-memscope-growth__context',
            `${shrinking ? 'removed' : 'added'} between snapshot #1 ` +
              `and #${rows.length}`,
          ),
          ratePerHour !== undefined &&
            m(
              'span.pf-memscope-growth__context',
              `· ${formatDelta(ratePerHour)}/hour`,
            ),
        ]),
        contribTotal > 0 &&
          m(
            '.pf-memscope-growth__bar',
            contributors.map((c) =>
              m('.pf-memscope-growth__segment', {
                style: {
                  width: `${(Math.abs(c.delta) / contribTotal) * 100}%`,
                  background: c.color,
                },
              }),
            ),
          ),
        m(
          '.pf-memscope-growth__legend',
          categories.map((c) =>
            m('span.pf-memscope-growth__legend-item', [
              m('span.pf-memscope-growth__swatch', {
                style: {background: c.color},
              }),
              m('span', c.name),
              m('span.pf-memscope-growth__legend-delta', formatDelta(c.delta)),
            ]),
          ),
        ),
      ]),
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

    return [
      this.renderScoreCards(vm),
      m(
        '.pf-memscope-charts',
        this.renderGrowthCard(data),
        this.renderMemoryMap(trace, vm),
        this.renderJavaSection(trace, vm),
        this.renderBitmapsSection(vm),
        this.renderNativeSection(vm),
      ),
    ];
  }
}
