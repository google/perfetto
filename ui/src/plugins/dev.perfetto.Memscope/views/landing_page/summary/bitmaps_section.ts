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

// "What about bitmaps?" — reachable android.graphics.Bitmap instances grouped
// by dimensions and storage backing, from the latest heap dump of one process.
// Self-contained: owns its query slot and loading logic.

import m from 'mithril';
import {QuerySlot} from '../../../../../base/query_slot';
import {Time, type time} from '../../../../../base/time';
import type {Trace} from '../../../../../public/trace';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../../../../trace_processor/query_result';
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
import {nearestByTs} from '../selection';
import {
  deltaCell,
  emptyPanel,
  loadingPanel,
  shortClassName,
  topTable,
} from '../section_widgets';
import {ShareBar} from '../../../components/share_bar';
import {Stack} from '../../../../../widgets/stack';
import {BillboardStrip} from '../../../components/billboard';

const TITLE = 'What about bitmaps?';
const SUBTITLE =
  'Usually the largest and most reducible cost on Android — pulled out on ' +
  'their own.';

// One reachable-bitmap group at the latest dump: (dimensions, storage backing)
// with its count and sizes. width/height are undefined on proto-format heap
// graphs (only ART HPROF dumps record field values).
interface BitmapGroup {
  width?: number;
  height?: number;
  storage?: string;
  count: number;
  selfSize: number;
  nativeSize: number;
}

interface BitmapsData {
  // All dumps with reachable bitmaps, ts-ascending (empty → none).
  readonly dumps: {ts: time}[];
  // Bitmap groups per dump, keyed by dump ts (raw bigint).
  readonly groupsByTs: ReadonlyMap<bigint, BitmapGroup[]>;
  // Reachable Java heap + registered native per dump (for the "share of Java
  // retained" card), keyed by dump ts.
  readonly javaRetainedByTs: ReadonlyMap<bigint, number>;
  // Nearest non-library class retaining the bitmaps (last dump), if resolvable.
  readonly bitmapRetainer?: string;
}

async function loadBitmapsData(
  trace: Trace,
  upid: number,
): Promise<BitmapsData> {
  await trace.engine.query(
    'INCLUDE PERFETTO MODULE android.memory.heap_graph.bitmap;',
  );
  const groupsByTs = new Map<bigint, BitmapGroup[]>();
  const res = await trace.engine.query(`
    SELECT
      b.graph_sample_ts AS ts,
      b.width AS width,
      b.height AS height,
      b.bitmap_storage_type AS storage,
      COUNT(*) AS cnt,
      CAST(ifnull(SUM(b.self_size), 0) AS INT) AS self_size,
      CAST(ifnull(SUM(b.native_size), 0) AS INT) AS native_size
    FROM heap_graph_bitmaps b
    WHERE b.upid = ${upid} AND b.reachable
    GROUP BY b.graph_sample_ts, b.width, b.height, b.bitmap_storage_type
    ORDER BY b.graph_sample_ts ASC
  `);
  for (
    const it = res.iter({
      ts: LONG,
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
    let list = groupsByTs.get(it.ts);
    if (list === undefined) {
      list = [];
      groupsByTs.set(it.ts, list);
    }
    list.push({
      width: it.width ?? undefined,
      height: it.height ?? undefined,
      storage: it.storage ?? undefined,
      count: it.cnt,
      selfSize: it.self_size,
      nativeSize: it.native_size,
    });
  }
  const dumps = Array.from(groupsByTs.keys())
    .sort((a, b) => (a < b ? -1 : a > b ? 1 : 0))
    .map((ts) => ({ts: Time.fromRaw(ts)}));

  const javaRetainedByTs = new Map<bigint, number>();
  try {
    await trace.engine.query(
      'INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_stats;',
    );
    const statsRes = await trace.engine.query(`
      SELECT
        s.graph_sample_ts AS ts,
        CAST(s.reachable_heap_size + s.reachable_native_alloc_registry_size
          AS INT) AS retained
      FROM android_heap_graph_stats s
      WHERE s.upid = ${upid}
    `);
    for (
      const it = statsRes.iter({ts: LONG, retained: NUM});
      it.valid();
      it.next()
    ) {
      javaRetainedByTs.set(it.ts, Math.max(0, it.retained));
    }
  } catch {
    // No heap-graph stats — leave the "of Java retained" card hidden.
  }

  const bitmapRetainer = await loadBitmapRetainer(trace, upid);

  return {dumps, groupsByTs, javaRetainedByTs, bitmapRetainer};
}

// The nearest non-library class that dominates the reachable Bitmap instances
// at the latest dump (the app-side owner up the dominator chain).
async function loadBitmapRetainer(
  trace: Trace,
  upid: number,
): Promise<string | undefined> {
  try {
    await trace.engine.query(
      'INCLUDE PERFETTO MODULE android.memory.heap_graph.raw_dominator_tree;',
    );
    const res = await trace.engine.query(`
      WITH RECURSIVE
      last_ts AS (
        SELECT MAX(graph_sample_ts) AS ts
        FROM heap_graph_object WHERE upid = ${upid}
      ),
      ck AS (
        SELECT c.id,
          CASE WHEN c.name GLOB 'java.*' OR c.name GLOB 'javax.*'
            OR c.name GLOB 'kotlin.*' OR c.name GLOB 'kotlinx.*'
            OR c.name GLOB 'dalvik.*' OR c.name GLOB 'sun.*'
            OR c.name GLOB 'libcore.*' OR c.name GLOB 'android.*'
            OR c.name GLOB 'androidx.*' OR c.name GLOB 'com.android.*'
            OR c.name GLOB 'com.google.android.*'
            OR c.name LIKE '%[]%'
            OR c.name NOT GLOB '*.*'
            THEN 0 ELSE 1 END AS is_app,
          c.name AS name
        FROM heap_graph_class c
      ),
      obj AS (
        SELECT o.id, ck.is_app, ck.name AS class_name,
          o.self_size + o.native_size AS size
        FROM heap_graph_object o
        JOIN last_ts l ON o.graph_sample_ts = l.ts
        JOIN ck ON ck.id = o.type_id
        WHERE o.upid = ${upid} AND o.reachable
      ),
      walk(seed_size, cur_id, depth) AS (
        SELECT ob.size, d.idom_id, 1
        FROM obj ob
        JOIN _raw_heap_graph_dominator_tree d ON d.id = ob.id
        WHERE ob.class_name = 'android.graphics.Bitmap' AND d.idom_id IS NOT NULL
        UNION ALL
        SELECT w.seed_size, d.idom_id, w.depth + 1
        FROM walk w
        JOIN obj cur ON cur.id = w.cur_id
        JOIN _raw_heap_graph_dominator_tree d ON d.id = w.cur_id
        WHERE cur.is_app = 0 AND d.idom_id IS NOT NULL AND w.depth < 24
      ),
      hits AS (
        SELECT cur.class_name AS retainer, w.seed_size,
          ROW_NUMBER() OVER (PARTITION BY w.cur_id ORDER BY w.depth) AS rn
        FROM walk w
        JOIN obj cur ON cur.id = w.cur_id
        WHERE cur.is_app = 1
      )
      SELECT retainer FROM hits WHERE rn = 1
      GROUP BY retainer
      ORDER BY SUM(seed_size) DESC
      LIMIT 1
    `);
    const it = res.iter({retainer: STR});
    if (it.valid()) return it.retainer;
  } catch {
    // No dominator tree — skip the retainer note.
  }
  return undefined;
}

// Pixel bytes for non-heap backings (ashmem / hardware) are not part of
// self/native size — estimate them as w*h*4 when dimensions are known.
function bytesOf(g: BitmapGroup): number {
  const estPixels =
    g.storage !== 'heap' && g.width !== undefined && g.height !== undefined
      ? g.width * g.height * 4 * g.count
      : 0;
  return g.selfSize + g.nativeSize + estPixels;
}

// Group bitmaps by dimensions only (current dump's count + bytes per size).
function groupByDims(
  groups: BitmapGroup[],
): Map<string, {count: number; bytes: number}> {
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
  return byDims;
}

export interface BitmapsSectionAttrs {
  readonly trace: Trace;
  readonly upid: number;
  // The page-wide selection (by ts). Undefined → latest dump.
  readonly selTs?: time;
  readonly baseTs?: time;
}

export class BitmapsSection implements m.ClassComponent<BitmapsSectionAttrs> {
  private readonly slot = new QuerySlot<BitmapsData>();

  onremove() {
    this.slot.dispose();
  }

  view({attrs}: m.Vnode<BitmapsSectionAttrs>): m.Children {
    const {trace, upid, selTs, baseTs} = attrs;
    const data = this.slot.use({
      key: {traceId: trace.traceInfo.uuid, upid},
      queryFn: () => loadBitmapsData(trace, upid),
      // Emulate an empty response
      // queryFn: async () => ({
      //   dumps: [],
      //   groupsByTs: new Map<bigint, BitmapGroup[]>(),
      //   javaRetainedByTs: new Map<bigint, number>(),
      // }),
    }).data;
    if (data === undefined) {
      return loadingPanel({title: TITLE, subtitle: SUBTITLE}); // Still loading.
    }
    if (data.dumps.length === 0) {
      return emptyPanel({
        title: TITLE,
        subtitle: SUBTITLE,
        message: 'No reachable bitmaps found',
        detail:
          'This trace has no Java heap graph, or the process allocated none.',
      });
    }

    // Resolve the selection to dumps. No selection → latest.
    const cur = nearestByTs(data.dumps, selTs)!;
    const baseDump =
      baseTs !== undefined ? nearestByTs(data.dumps, baseTs) : undefined;
    const comparing = baseDump !== undefined && baseDump.ts !== cur.ts;
    const groups = data.groupsByTs.get(cur.ts) ?? [];
    const baseGroups = comparing
      ? (data.groupsByTs.get(baseDump.ts) ?? [])
      : [];

    const totalCount = groups.reduce((s, g) => s + g.count, 0);
    const totalBytes = groups.reduce((s, g) => s + bytesOf(g), 0);
    const baseCount = baseGroups.reduce((s, g) => s + g.count, 0);
    const baseBytes = baseGroups.reduce((s, g) => s + bytesOf(g), 0);
    const heapBytes = groups
      .filter((g) => g.storage === 'heap')
      .reduce((s, g) => s + g.selfSize + g.nativeSize, 0);
    const ashmemBytes = groups
      .filter((g) => g.storage === 'ashmem')
      .reduce((s, g) => s + bytesOf(g), 0);

    const byDims = groupByDims(groups);
    const byDimsBase = comparing ? groupByDims(baseGroups) : undefined;
    const dims = Array.from(byDims.entries()).map(([key, e]) => ({key, ...e}));
    const bySize = dims.slice().sort((a, b) => b.bytes - a.bytes);
    const byCount = dims.slice().sort((a, b) => b.count - a.count);
    const hasDims = dims.some((d) => d.key !== 'unknown');
    const largest = bySize[0];

    const javaRetained = data.javaRetainedByTs.get(cur.ts) ?? 0;
    // Bitmap heap-self + registered-native bytes (excludes the estimated ashmem
    // pixels, which aren't part of the Java retained total) as a % of the
    // dump's reachable heap + native, with the baseline equivalent for the diff.
    const bitmapHeapNative = groups.reduce(
      (s, g) => s + g.selfSize + g.nativeSize,
      0,
    );
    const javaRetainedPct =
      javaRetained > 0 ? (bitmapHeapNative / javaRetained) * 100 : 0;
    const baseJavaRetained = comparing
      ? (data.javaRetainedByTs.get(baseDump.ts) ?? 0)
      : 0;
    const baseBitmapHeapNative = baseGroups.reduce(
      (s, g) => s + g.selfSize + g.nativeSize,
      0,
    );
    const baseJavaRetainedPct =
      baseJavaRetained > 0
        ? (baseBitmapHeapNative / baseJavaRetained) * 100
        : 0;
    const javaRetainedPtsDelta = javaRetainedPct - baseJavaRetainedPct;
    const retainerNote: m.Children =
      data.bitmapRetainer !== undefined
        ? [
            ' Mostly retained by ',
            m('b', shortClassName(data.bitmapRetainer)),
            '.',
          ]
        : '';

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
          retainerNote,
        ]
      : [
          'This trace format does not record bitmap dimensions — only ' +
            'counts and sizes are available.',
        ];

    const dimRows = (list: {key: string; count: number; bytes: number}[]) =>
      list.slice(0, 5).map((d) => {
        const base = byDimsBase?.get(d.key);
        const shareFrac = totalBytes > 0 ? d.bytes / totalBytes : 0;
        const baseShareFrac =
          base !== undefined && baseBytes > 0 ? base.bytes / baseBytes : 0;
        return [
          d.key,
          deltaCell(
            d.count.toLocaleString(),
            base !== undefined ? d.count - base.count : undefined,
            comparing,
            formatCountDelta,
          ),
          deltaCell(
            formatBytes(d.bytes),
            base !== undefined ? d.bytes - base.bytes : undefined,
            comparing,
          ),
          // Share of all bitmap bytes, with the change in share (percentage
          // points, this snapshot's total vs the baseline's) below in diff mode.
          deltaCell(
            m(ShareBar, {frac: shareFrac}),
            comparing ? (shareFrac - baseShareFrac) * 100 : undefined,
            comparing,
            (n) =>
              n === 0 ? '±0 pts' : `${n > 0 ? '+' : ''}${n.toFixed(1)} pts`,
          ),
        ];
      });

    const tableSubtitle = comparing
      ? 'grouped by dimensions · Δ vs baseline'
      : 'grouped by dimensions';

    return m(
      Panel,
      m(Panel.Header, {title: TITLE, subtitle: SUBTITLE}),
      m(
        Panel.Body,
        m(Stack, {spacing: 'large'}, [
          m(Callout, {intent: Intent.Primary}, insight),
          m(BillboardStrip, [
            statCard([
              {
                label: 'Total bitmaps',
                value: totalCount.toLocaleString(),
                sub: comparing
                  ? deltaText(
                      totalCount - baseCount,
                      `Δ ${formatCountDelta(totalCount - baseCount)} vs baseline`,
                    )
                  : 'live android.graphics.Bitmap',
              },
            ]),
            statCard([
              {
                label: 'Bitmap memory',
                value: formatBytes(totalBytes),
                sub: comparing
                  ? deltaText(
                      totalBytes - baseBytes,
                      `Δ ${formatDelta(totalBytes - baseBytes)} vs baseline`,
                    )
                  : `${formatBytes(heapBytes)} heap · ${formatBytes(
                      ashmemBytes,
                    )} ashmem (est.)`,
              },
            ]),
            hasDims &&
              statCard([
                {
                  label: 'Largest group',
                  value: formatBytes(largest.bytes),
                  sub: `${largest.key} ×${largest.count}`,
                },
              ]),
            javaRetained > 0 &&
              statCard([
                {
                  label: 'Of Java retained',
                  value: `${Math.round(javaRetainedPct)}%`,
                  sub: comparing
                    ? deltaText(
                        javaRetainedPtsDelta,
                        `Δ ${javaRetainedPtsDelta >= 0 ? '+' : ''}${Math.round(
                          javaRetainedPtsDelta,
                        )} pts vs baseline`,
                      )
                    : 'share of heap + native',
                },
              ]),
          ]),
          hasDims &&
            m('.pf-memscope-tables', [
              topTable({
                title: 'Largest bitmaps',
                subtitle: tableSubtitle,
                cols: [
                  {label: 'Dimensions'},
                  {label: 'Count', num: true},
                  {label: 'Size', num: true},
                  {label: 'Share of all bitmap', num: true},
                ],
                rows: dimRows(bySize),
              }),
              topTable({
                title: 'Most frequent bitmaps',
                subtitle: tableSubtitle,
                cols: [
                  {label: 'Dimensions'},
                  {label: 'Count', num: true},
                  {label: 'Size', num: true},
                  {label: 'Share of all bitmap', num: true},
                ],
                rows: dimRows(byCount),
              }),
            ]),
        ]),
      ),
    );
  }
}
