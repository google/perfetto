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

// "How much Java memory did you use, and where did it go?" — from the full
// heap graph: reachability, totals and the three top-classes tables, for the
// latest heap dump of one process. Self-contained: owns its query slot and
// loading logic.

import m from 'mithril';
import {Icons} from '../../../../../base/semantic_icons';
import {AsyncMemo} from '../../../../../base/async_memo';
import {Time, type time} from '../../../../../base/time';
import type {Trace} from '../../../../../public/trace';
import {LONG, NUM, STR} from '../../../../../trace_processor/query_result';
import {Anchor} from '../../../../../widgets/anchor';
import {Panel} from '../../../components/panel';
import {Callout} from '../../../components/callout';
import {Intent} from '../../../../../widgets/common';
import {
  deltaText,
  formatBytes,
  formatCountDelta,
  formatDelta,
  statCard,
} from '../mem_format';
import {Ratio} from '../../../components/ratio';
import {findProcessTrack, showInTimelineLink} from '../process_links';
import {nearestByTs} from '../selection';
import {
  classNameCell,
  deltaCell,
  emptyPanel,
  loadingPanel,
  shortClassName,
  topTable,
} from '../section_widgets';
import {ShareBar} from '../../../components/share_bar';
import {Stack} from '../../../../../widgets/stack';
import {BillboardStrip} from '../../../components/billboard';

const TITLE = 'How much Java memory did you use, and where did it go?';
const SUBTITLE =
  'From the full heap graph — complete and exact, with retention but no ' +
  'callstacks.';

// Per-class aggregation row at a dump. All sizes/counts are reachable-only (see
// landing_page_spec.md): unreachable garbage is excluded so the three tables'
// columns are comparable and retained >= shallow always holds. rnRetained/
// rnShallow/rnCount are the class's rank under each of the three orderings.
interface ClassAggRow {
  typeName: string;
  // Reachable instance count of this class.
  reachableObjCount: number;
  // Shallow: sum of self_size over reachable instances (Java heap only).
  reachableSizeBytes: number;
  // Native self: registered native owned by this class's reachable instances.
  reachableNativeSizeBytes: number;
  // Retained = dominatedSizeBytes + dominatedNativeSizeBytes (Java + native).
  dominatedSizeBytes: number;
  dominatedNativeSizeBytes: number;
  rnRetained: number; // by retained (dominated Java + native)
  rnShallow: number; // by shallow (reachable self)
  rnCount: number; // by reachable instance count
}

// Per-dump heap-graph stats.
interface DumpStats {
  readonly ts: time;
  readonly totalHeapSize: number;
  readonly totalObjCount: number;
  readonly reachableHeapSize: number;
  readonly reachableNativeSize: number;
  readonly reachableObjCount: number;
  // MIN(heap_graph_object.id) for this dump — the HeapProfile track event id
  // used to select the dump on the timeline.
  readonly eventId: number;
}

// One app-side owner of a (library) class's instances: the nearest app class up
// the dominator chain, and the bytes of that class attributed to it.
export interface ClassRetainer {
  readonly name: string;
  readonly bytes: number;
}

interface JavaData {
  // All dumps for this process, ts-ascending (empty → no heap graph).
  readonly dumps: DumpStats[];
  // Top classes per dump, keyed by dump ts (raw bigint).
  readonly classesByTs: ReadonlyMap<bigint, ClassAggRow[]>;
  // App-side retainers per class (last dump): the distinct nearest app-side
  // dominators of a library class's instances, each with the bytes attributed
  // to it, largest first. A class held through several owners gets several
  // entries instead of a single (misleading) "via".
  readonly retainerOf: ReadonlyMap<string, ReadonlyArray<ClassRetainer>>;
  // ART images + JIT cache resident bytes, one entry per smaps snapshot
  // (ts-ascending). Lets the card align to the selected/baseline dump and diff.
  readonly artByTs: readonly {ts: time; art: number}[];
}

async function loadJavaData(trace: Trace, upid: number): Promise<JavaData> {
  await trace.engine.query(
    'INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_stats;',
  );
  // MIN(object id) per dump — the HeapProfile track event id for that dump
  // (mirrors how dev.perfetto.HeapProfile builds its timeline events).
  const eventIdByTs = new Map<bigint, number>();
  const eventRes = await trace.engine.query(`
    SELECT graph_sample_ts AS ts, MIN(id) AS event_id
    FROM heap_graph_object
    WHERE upid = ${upid}
    GROUP BY graph_sample_ts
  `);
  for (
    const it = eventRes.iter({ts: LONG, event_id: NUM});
    it.valid();
    it.next()
  ) {
    eventIdByTs.set(it.ts, it.event_id);
  }

  const dumps: DumpStats[] = [];
  const statsRes = await trace.engine.query(`
    SELECT
      s.graph_sample_ts AS ts,
      s.total_heap_size AS total_heap_size,
      s.total_obj_count AS total_obj_count,
      s.reachable_heap_size AS reachable_heap_size,
      s.reachable_native_alloc_registry_size AS reachable_native_size,
      s.reachable_obj_count AS reachable_obj_count
    FROM android_heap_graph_stats s
    WHERE s.upid = ${upid}
    ORDER BY s.graph_sample_ts ASC
  `);
  for (
    const it = statsRes.iter({
      ts: LONG,
      total_heap_size: NUM,
      total_obj_count: NUM,
      reachable_heap_size: NUM,
      reachable_native_size: NUM,
      reachable_obj_count: NUM,
    });
    it.valid();
    it.next()
  ) {
    dumps.push({
      ts: Time.fromRaw(it.ts),
      totalHeapSize: it.total_heap_size,
      totalObjCount: it.total_obj_count,
      reachableHeapSize: it.reachable_heap_size,
      reachableNativeSize: it.reachable_native_size,
      reachableObjCount: it.reachable_obj_count,
      eventId: eventIdByTs.get(it.ts) ?? -1,
    });
  }
  const classesByTs = new Map<bigint, ClassAggRow[]>();
  if (dumps.length === 0) {
    return {dumps, classesByTs, retainerOf: new Map(), artByTs: []};
  }

  // Top classes (per the three orderings) at every dump, so picking any
  // snapshot (or diffing two) is in-memory.
  await trace.engine.query(
    'INCLUDE PERFETTO MODULE ' +
      'android.memory.heap_graph.heap_graph_class_aggregation;',
  );
  const aggRes = await trace.engine.query(`
    WITH ranked AS (
      SELECT
        a.*,
        ROW_NUMBER() OVER (
          PARTITION BY a.graph_sample_ts
          ORDER BY a.dominated_size_bytes + a.dominated_native_size_bytes DESC
        ) AS rn_retained,
        ROW_NUMBER() OVER (
          PARTITION BY a.graph_sample_ts
          ORDER BY a.reachable_size_bytes DESC
        ) AS rn_shallow,
        ROW_NUMBER() OVER (
          PARTITION BY a.graph_sample_ts ORDER BY a.reachable_obj_count DESC
        ) AS rn_count
      FROM android_heap_graph_class_aggregation a
      WHERE a.upid = ${upid}
    )
    SELECT
      r.graph_sample_ts AS ts,
      r.type_name AS type_name,
      r.reachable_obj_count AS reachable_obj_count,
      r.reachable_size_bytes AS reachable_size_bytes,
      r.reachable_native_size_bytes AS reachable_native_size_bytes,
      r.dominated_size_bytes AS dominated_size_bytes,
      r.dominated_native_size_bytes AS dominated_native_size_bytes,
      r.rn_retained AS rn_retained,
      r.rn_shallow AS rn_shallow,
      r.rn_count AS rn_count
    FROM ranked r
    WHERE r.rn_retained <= 8 OR r.rn_shallow <= 8 OR r.rn_count <= 8
  `);
  for (
    const it = aggRes.iter({
      ts: LONG,
      type_name: STR,
      reachable_obj_count: NUM,
      reachable_size_bytes: NUM,
      reachable_native_size_bytes: NUM,
      dominated_size_bytes: NUM,
      dominated_native_size_bytes: NUM,
      rn_retained: NUM,
      rn_shallow: NUM,
      rn_count: NUM,
    });
    it.valid();
    it.next()
  ) {
    let list = classesByTs.get(it.ts);
    if (list === undefined) {
      list = [];
      classesByTs.set(it.ts, list);
    }
    list.push({
      typeName: it.type_name,
      reachableObjCount: it.reachable_obj_count,
      reachableSizeBytes: it.reachable_size_bytes,
      reachableNativeSizeBytes: it.reachable_native_size_bytes,
      dominatedSizeBytes: it.dominated_size_bytes,
      dominatedNativeSizeBytes: it.dominated_native_size_bytes,
      rnRetained: it.rn_retained,
      rnShallow: it.rn_shallow,
      rnCount: it.rn_count,
    });
  }

  const retainerOf = await loadRetainers(trace, upid);
  const artByTs = await loadArtByTs(trace, upid);

  return {dumps, classesByTs, retainerOf, artByTs};
}

// For each library class at the latest dump, the app-side classes that dominate
// its instances, walking up the dominator tree to the nearest non-library
// (app/third-party) ancestor of each object. Instances of one class can resolve
// to different owners, so we report up to RETAINER_MAX_VIAS of them, each with
// the bytes attributed to it (the shallow size of the library objects that
// terminate at that owner — a clean partition, not the overlapping dominated
// sizes shown in the table). When the chain reaches the super root without
// passing through an app class — i.e. the object is held directly by the GC
// root set — the retainer is reported as "GC root". Wrapped defensively: a heap
// graph without a dominator tree yields an empty map.
//
// At most this many distinct owners are surfaced per class, ...
const RETAINER_MAX_VIAS = 3;
// ...and owners contributing less than this fraction of the class's largest
// owner are dropped as noise.
const RETAINER_MIN_SHARE = 0.1;
async function loadRetainers(
  trace: Trace,
  upid: number,
): Promise<ReadonlyMap<string, ReadonlyArray<ClassRetainer>>> {
  const retainerOf = new Map<string, ClassRetainer[]>();
  try {
    await trace.engine.query(
      'INCLUDE PERFETTO MODULE android.memory.heap_graph.raw_dominator_tree;',
    );
    await trace.engine.query('INCLUDE PERFETTO MODULE graphs.scan;');
    const res = await trace.engine.query(`
      WITH
      last_ts AS (
        SELECT MAX(graph_sample_ts) AS ts
        FROM heap_graph_object WHERE upid = ${upid}
      ),
      ck AS (
        -- Classify by the *wrapped* name: a class object is named
        -- "java.lang.Class<X>" and holds X's statics, so an app class's class
        -- object must count as app (an app-side owner), not library. The
        -- original name is kept for display/joins; only is_app uses the
        -- unwrapped form.
        SELECT id, name,
          CASE WHEN cname GLOB 'java.*' OR cname GLOB 'javax.*'
            OR cname GLOB 'kotlin.*' OR cname GLOB 'kotlinx.*'
            OR cname GLOB 'dalvik.*' OR cname GLOB 'sun.*'
            OR cname GLOB 'libcore.*' OR cname GLOB 'android.*'
            OR cname GLOB 'androidx.*' OR cname GLOB 'com.android.*'
            OR cname GLOB 'com.google.android.*'
            OR cname LIKE '%[]%'
            OR cname NOT GLOB '*.*'
            THEN 0 ELSE 1 END AS is_app
        FROM (
          SELECT c.id, c.name AS name,
            CASE WHEN c.name GLOB 'java.lang.Class<*>'
              THEN substr(c.name, 17, length(c.name) - 17)
              ELSE c.name END AS cname
          FROM heap_graph_class c
        )
      ),
      obj AS (
        SELECT o.id, ck.is_app, ck.name AS class_name,
          o.self_size + o.native_size AS size
        FROM heap_graph_object o
        JOIN last_ts l ON o.graph_sample_ts = l.ts
        JOIN ck ON ck.id = o.type_id
        WHERE o.upid = ${upid} AND o.reachable
      ),
      -- Traverse the dominator tree once, from roots to leaves, carrying the
      -- nearest app-side ancestor. This avoids a separate upward walk (and the
      -- same ancestor joins) for every library object.
      ownership AS (
        SELECT id, nearest_app_id
        FROM _graph_scan!(
          (
            SELECT d.idom_id AS source_node_id, d.id AS dest_node_id
            FROM _raw_heap_graph_dominator_tree d
            JOIN obj ob ON ob.id = d.id
            WHERE d.idom_id IS NOT NULL
          ),
          (
            SELECT ob.id,
              CASE WHEN ob.is_app = 1 THEN ob.id ELSE -1 END AS nearest_app_id
            FROM obj ob
            JOIN _raw_heap_graph_dominator_tree d ON d.id = ob.id
            WHERE d.idom_id IS NULL
          ),
          (nearest_app_id),
          (
            SELECT t.id,
              CASE WHEN child_ck.is_app = 1 THEN t.id
                ELSE t.nearest_app_id END AS nearest_app_id
            FROM $table t
            JOIN heap_graph_object o ON o.id = t.id
            JOIN (
              SELECT id,
                CASE WHEN cname GLOB "java.*" OR cname GLOB "javax.*"
                  OR cname GLOB "kotlin.*" OR cname GLOB "kotlinx.*"
                  OR cname GLOB "dalvik.*" OR cname GLOB "sun.*"
                  OR cname GLOB "libcore.*" OR cname GLOB "android.*"
                  OR cname GLOB "androidx.*" OR cname GLOB "com.android.*"
                  OR cname GLOB "com.google.android.*"
                  OR cname LIKE "%[]%"
                  OR cname NOT GLOB "*.*"
                  THEN 0 ELSE 1 END AS is_app
              FROM (
                SELECT c.id,
                  CASE WHEN c.name GLOB "java.lang.Class<*>"
                    THEN substr(c.name, 17, length(c.name) - 17)
                    ELSE c.name END AS cname
                FROM heap_graph_class c
              )
            ) child_ck ON child_ck.id = o.type_id
          )
        )
      ),
      hits AS (
        SELECT ob.class_name AS owned_class, ob.size AS seed_size,
          CASE
            WHEN own.nearest_app_id = -1 THEN 'GC root'
            WHEN retainer_ck.name GLOB 'java.lang.Class<*>'
              THEN substr(retainer_ck.name, 17, length(retainer_ck.name) - 17)
            ELSE retainer_ck.name
          END AS retainer
        FROM ownership own
        JOIN obj ob ON ob.id = own.id
        LEFT JOIN heap_graph_object retainer
          ON retainer.id = own.nearest_app_id
        LEFT JOIN ck retainer_ck ON retainer_ck.id = retainer.type_id
        WHERE ob.is_app = 0
      ),
      -- Rank the distinct retainers of each class by attributed bytes, largest
      -- first, so the UI can show the few dominant owners instead of just one.
      -- "GC root" ranks last on ties so a real app owner wins an equal split.
      agg AS (
        SELECT owned_class, retainer, SUM(seed_size) AS bytes,
          ROW_NUMBER() OVER (
            PARTITION BY owned_class
            ORDER BY SUM(seed_size) DESC, (retainer = 'GC root') ASC
          ) AS rrn
        FROM hits
        GROUP BY owned_class, retainer
      )
      SELECT a.owned_class AS type_name, a.retainer AS retainer_name,
        a.bytes AS bytes
      FROM agg a
      WHERE a.rrn <= ${RETAINER_MAX_VIAS}
      ORDER BY a.owned_class, a.rrn
    `);
    for (
      const it = res.iter({type_name: STR, retainer_name: STR, bytes: NUM});
      it.valid();
      it.next()
    ) {
      let list = retainerOf.get(it.type_name);
      if (list === undefined) {
        list = [];
        retainerOf.set(it.type_name, list);
      }
      list.push({name: it.retainer_name, bytes: it.bytes});
    }
    // Drop trailing owners that are tiny next to the class's biggest one; the
    // SQL already ordered each list largest-first.
    for (const [cls, list] of retainerOf) {
      const cutoff = list[0].bytes * RETAINER_MIN_SHARE;
      retainerOf.set(
        cls,
        list.filter((r) => r.bytes >= cutoff),
      );
    }
  } catch {
    // No dominator tree (e.g. non-Android heap graph) — skip the annotation.
  }
  return retainerOf;
}

// ART images + JIT cache resident bytes per smaps snapshot (ts-ascending), so
// the card can align to the selected/baseline dump and diff between them.
async function loadArtByTs(
  trace: Trace,
  upid: number,
): Promise<{ts: time; art: number}[]> {
  const res = await trace.engine.query(`
    SELECT s.ts AS ts, CAST(ifnull(SUM(s.rss_kb), 0) * 1024 AS INT) AS art
    FROM profiler_smaps s
    WHERE s.upid = ${upid}
      AND (s.path GLOB '*.art*' OR s.path GLOB '*.oat*'
        OR s.path GLOB '*.odex*' OR s.path GLOB '*.vdex*'
        OR s.path GLOB '*dalvik-jit*')
    GROUP BY s.ts
    ORDER BY s.ts ASC
  `);
  const out: {ts: time; art: number}[] = [];
  for (const it = res.iter({ts: LONG, art: NUM}); it.valid(); it.next()) {
    out.push({ts: Time.fromRaw(it.ts), art: it.art});
  }
  return out;
}

export interface JavaSectionAttrs {
  readonly trace: Trace;
  readonly upid: number;
  // The page-wide selection (by ts). Undefined → latest dump.
  readonly selTs?: time;
  readonly baseTs?: time;
}

export class JavaSection implements m.ClassComponent<JavaSectionAttrs> {
  private readonly slot = new AsyncMemo<JavaData>();

  onremove() {
    this.slot.dispose();
  }

  view({attrs}: m.Vnode<JavaSectionAttrs>): m.Children {
    const {trace, upid, selTs, baseTs} = attrs;
    const data = this.slot.use({
      key: {traceId: trace.traceInfo.uuid, upid},
      compute: () => loadJavaData(trace, upid),
      // Emulate no data available
      // queryFn: async () => ({
      //   dumps: [],
      //   classesByTs: new Map<bigint, ClassAggRow[]>(),
      //   retainerOf: new Map<string, string>(),
      //   artByTs: [],
      // }),
    }).data;
    if (data === undefined) {
      return loadingPanel({title: TITLE, subtitle: SUBTITLE}); // Still loading.
    }
    if (data.dumps.length === 0) {
      return emptyPanel({
        title: TITLE,
        subtitle: SUBTITLE,
        message: 'No Java heap graph in this trace for this process',
        detail: 'Capture a java_hprof dump to see the managed heap',
      });
    }

    // Resolve the selection to dumps (nearest by ts). No selection → latest.
    const cur = nearestByTs(data.dumps, selTs)!;
    const baseDump =
      baseTs !== undefined ? nearestByTs(data.dumps, baseTs) : undefined;
    // Both endpoints can resolve to the same dump (dumps are sparser than
    // smaps snapshots) — that's not a comparison.
    const baseStats =
      baseDump !== undefined && baseDump.ts !== cur.ts ? baseDump : undefined;
    const comparing = baseStats !== undefined;
    const selIdx = data.dumps.indexOf(cur);
    const baseIdx =
      baseStats !== undefined ? data.dumps.indexOf(baseStats) : undefined;

    // ART overhead is smaps-sourced, so align to the smaps snapshot nearest
    // each dump's ts (rather than always the latest) so it diffs alongside the
    // heap-graph cards in compare mode.
    const curArt = nearestByTs(data.artByTs, cur.ts)?.art;
    const baseArt = comparing
      ? nearestByTs(data.artByTs, baseStats.ts)?.art
      : undefined;

    const classes = data.classesByTs.get(cur.ts) ?? [];
    const baseClassByName = new Map(
      comparing
        ? (data.classesByTs.get(baseStats.ts) ?? []).map(
            (c) => [c.typeName, c] as const,
          )
        : [],
    );
    const withDelta = (
      text: m.Children,
      delta: number | undefined,
      fmt: (n: number) => string = formatDelta,
    ): m.Children => deltaCell(text, delta, comparing, fmt);

    const tOf = (ts: time) =>
      (Number(ts - trace.traceInfo.start) / 1e9).toFixed(1);

    const reachableTotal = cur.reachableHeapSize + cur.reachableNativeSize;
    const baseReachableTotal = comparing
      ? baseStats.reachableHeapSize + baseStats.reachableNativeSize
      : 0;

    // A share-% cell that, in compare mode, shows the change in share below the
    // bar as signed percentage points (cur share − baseline share).
    const shareCell = (
      value: number,
      total: number,
      baseValue: number | undefined,
      baseTotal: number,
    ): m.Children => {
      const frac = total > 0 ? value / total : 0;
      if (!comparing) return m(ShareBar, {frac});
      const baseFrac =
        baseValue !== undefined && baseTotal > 0 ? baseValue / baseTotal : 0;
      return withDelta(m(ShareBar, {frac}), (frac - baseFrac) * 100, (n) =>
        n === 0 ? '±0 pts' : `${n > 0 ? '+' : ''}${n.toFixed(1)} pts`,
      );
    };
    const unreachableHeap = cur.totalHeapSize - cur.reachableHeapSize;
    const reachPct =
      cur.totalHeapSize > 0
        ? (cur.reachableHeapSize / cur.totalHeapSize) * 100
        : 0;

    // Insight: the class with the largest retained (dominated) size.
    const topRetainer = classes
      .slice()
      .sort((a, b) => a.rnRetained - b.rnRetained)[0];
    const insight: m.Children = [];
    if (topRetainer !== undefined && reachableTotal > 0) {
      const retained =
        topRetainer.dominatedSizeBytes + topRetainer.dominatedNativeSizeBytes;
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
        '.',
      );
    }
    if (cur.totalHeapSize > 0) {
      insight.push(
        ' ',
        m('b', `${Math.round(100 - reachPct)}%`),
        ' of the heap is currently unreachable (awaiting GC).',
      );
    }

    // All three class tables show the same five reachable-only columns
    // (Class · Instances · Shallow · Retained · Share); only the ranking and
    // the share denominator differ. `shareMetric` selects the value the share
    // bar measures and `shareTotal`/`shareBaseTotal` its denominator.
    const classCols = [
      {label: 'Class'},
      {label: 'Instances', num: true},
      {label: 'Shallow', num: true},
      {label: 'Native', num: true},
      {label: 'Retained', num: true},
      {label: 'Share', num: true},
    ];
    const classRow = (
      c: ClassAggRow,
      shareMetric: (r: ClassAggRow) => number,
      shareTotal: number,
      shareBaseTotal: number,
    ): m.Children[] => {
      const b = baseClassByName.get(c.typeName);
      const retained = c.dominatedSizeBytes + c.dominatedNativeSizeBytes;
      const baseRetained =
        b !== undefined
          ? b.dominatedSizeBytes + b.dominatedNativeSizeBytes
          : undefined;
      return [
        classNameCell(c.typeName, data.retainerOf.get(c.typeName)),
        withDelta(
          c.reachableObjCount.toLocaleString(),
          b !== undefined
            ? c.reachableObjCount - b.reachableObjCount
            : undefined,
          formatCountDelta,
        ),
        withDelta(
          formatBytes(c.reachableSizeBytes),
          b !== undefined
            ? c.reachableSizeBytes - b.reachableSizeBytes
            : undefined,
        ),
        withDelta(
          c.reachableNativeSizeBytes > 0
            ? formatBytes(c.reachableNativeSizeBytes)
            : '—',
          b !== undefined
            ? c.reachableNativeSizeBytes - b.reachableNativeSizeBytes
            : undefined,
        ),
        withDelta(
          formatBytes(retained),
          baseRetained !== undefined ? retained - baseRetained : undefined,
        ),
        shareCell(
          shareMetric(c),
          shareTotal,
          b !== undefined ? shareMetric(b) : undefined,
          shareBaseTotal,
        ),
      ];
    };

    const retainedRows = classes
      .filter((c) => c.rnRetained <= 5)
      .sort((a, b) => a.rnRetained - b.rnRetained)
      .map((c) =>
        classRow(
          c,
          (r) => r.dominatedSizeBytes + r.dominatedNativeSizeBytes,
          reachableTotal,
          baseReachableTotal,
        ),
      );

    const shallowRows = classes
      .filter((c) => c.rnShallow <= 5)
      .sort((a, b) => a.rnShallow - b.rnShallow)
      .map((c) =>
        classRow(
          c,
          (r) => r.reachableSizeBytes,
          cur.reachableHeapSize,
          comparing ? baseStats.reachableHeapSize : 0,
        ),
      );

    const countRows = classes
      .filter((c) => c.rnCount <= 5)
      .sort((a, b) => a.rnCount - b.rnCount)
      .map((c) =>
        classRow(
          c,
          (r) => r.reachableObjCount,
          cur.reachableObjCount,
          comparing ? baseStats.reachableObjCount : 0,
        ),
      );

    return m(
      Panel,
      m(Panel.Header, {
        title: TITLE,
        subtitle: SUBTITLE,
        controls: [
          showInTimelineLink(
            trace,
            findProcessTrack(trace, upid, (k) => k === 'java_heap_graph')?.uri,
            cur.eventId,
          ),
          m(
            Anchor,
            {
              disabled: true,
              title:
                'Direct linking to specific heap dumps in HDE is not yet ' +
                'supported',
              icon: Icons.ExternalLink,
            },
            'Show in Heap Dump Explorer',
          ),
        ],
      }),
      m(
        Panel.Body,
        m(Stack, {spacing: 'large'}, [
          m('.pf-memscope-memmap__section-header', [
            m(
              'span.pf-memscope-memmap__section-title',
              {
                className: comparing
                  ? 'pf-memscope-memmap__section-title--diff'
                  : 'pf-memscope-memmap__section-title--straight',
              },
              comparing && baseStats !== undefined
                ? `Dump @ t=${tOf(baseStats.ts)}s → t=${tOf(cur.ts)}s`
                : `Dump @ t=${tOf(cur.ts)}s`,
            ),
            m(
              'span.pf-memscope-memmap__section-hint',
              comparing && baseIdx !== undefined
                ? `dumps #${baseIdx + 1} → #${selIdx + 1} · Δ vs baseline`
                : `dump #${selIdx + 1} of ${data.dumps.length}`,
            ),
          ]),
          insight.length > 0 && m(Callout, {intent: Intent.Primary}, insight),
          m(BillboardStrip, [
            statCard([
              {
                label: 'Heap',
                value: formatBytes(cur.totalHeapSize),
                sub: comparing
                  ? deltaText(
                      cur.totalHeapSize - baseStats.totalHeapSize,
                      `Δ ${formatDelta(cur.totalHeapSize - baseStats.totalHeapSize)} vs baseline`,
                    )
                  : 'reachable + unreachable',
              },
            ]),
            statCard([
              {
                label: 'Live objects',
                value: cur.totalObjCount.toLocaleString(),
                sub: comparing
                  ? deltaText(
                      cur.totalObjCount - baseStats.totalObjCount,
                      `Δ ${formatCountDelta(cur.totalObjCount - baseStats.totalObjCount)} vs baseline`,
                    )
                  : `${cur.reachableObjCount.toLocaleString()} reach · ` +
                    `${(
                      cur.totalObjCount - cur.reachableObjCount
                    ).toLocaleString()} unreach`,
              },
            ]),
            statCard([
              {
                label: 'Registered native',
                value: formatBytes(cur.reachableNativeSize),
                sub: comparing
                  ? deltaText(
                      cur.reachableNativeSize - baseStats.reachableNativeSize,
                      `Δ ${formatDelta(cur.reachableNativeSize - baseStats.reachableNativeSize)} vs baseline`,
                    )
                  : 'owned by Java (bitmaps, NIO)',
              },
            ]),
            curArt !== undefined &&
              curArt > 0 &&
              statCard([
                {
                  label: 'ART overhead',
                  value: formatBytes(curArt),
                  sub:
                    comparing && baseArt !== undefined
                      ? deltaText(
                          curArt - baseArt,
                          `Δ ${formatDelta(curArt - baseArt)} vs baseline`,
                        )
                      : '.art images + JIT cache',
                },
              ]),
          ]),
          m(Ratio, {
            label: 'Heap reachability',
            tooltip:
              'Reachable = objects a GC root still references. Unreachable ' +
              'objects are garbage awaiting collection.',
            pct: reachPct,
            headline: 'of the Java heap is reachable',
            color: '#f4b400',
            aLabel: 'Reachable',
            aBytes: cur.reachableHeapSize,
            bLabel: 'Unreachable',
            bBytes: unreachableHeap,
            ...(comparing
              ? {
                  aDelta: cur.reachableHeapSize - baseStats.reachableHeapSize,
                  bDelta:
                    unreachableHeap -
                    (baseStats.totalHeapSize - baseStats.reachableHeapSize),
                  pctDelta:
                    reachPct -
                    (baseStats.totalHeapSize > 0
                      ? (baseStats.reachableHeapSize /
                          baseStats.totalHeapSize) *
                        100
                      : 0),
                }
              : {}),
          }),
          m('.pf-memscope-tables', [
            retainedRows.length > 0 &&
              topTable({
                title: 'Top classes by retained size',
                cols: classCols,
                rows: retainedRows,
              }),
            shallowRows.length > 0 &&
              topTable({
                title: 'Top classes by shallow size',
                cols: classCols,
                rows: shallowRows,
              }),
            countRows.length > 0 &&
              topTable({
                title: 'Top classes by instance count',
                cols: classCols,
                rows: countRows,
              }),
          ]),
        ]),
      ),
    );
  }
}
