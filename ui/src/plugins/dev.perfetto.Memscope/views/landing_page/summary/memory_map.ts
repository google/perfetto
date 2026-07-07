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

// The "Where did all the memory go?" panel: a complete breakdown of resident
// memory for one process, from smaps, rendered as a nested flame-chart. The
// Native and Java blocks are optionally split a third level using cross-source
// figures (heapprofd unreleased / heap-graph reachable). Self-contained: owns
// its own query slot and loading logic and shows the latest smaps snapshot.

import m from 'mithril';
import {QuerySlot} from '../../../../../base/query_slot';
import {Time, type time} from '../../../../../base/time';
import {
  FlamegraphChart,
  type FlamegraphChartSegment,
} from '../../../../../components/widgets/charts/flamegraph_chart';
import type {Trace} from '../../../../../public/trace';
import {SMAPS_TRACK_KIND} from '../../../../../public/track_kinds';
import {LONG, NUM, STR} from '../../../../../trace_processor/query_result';
import {Callout} from '../../../components/callout';
import {Intent} from '../../../../../widgets/common';
import {Panel} from '../../../components/panel';
import {deltaText, formatBytes, formatDelta} from '../mem_format';
import {findProcessTrack, showInTimelineLink} from '../process_links';
import {nearestByTs} from '../selection';
import {emptyPanel, loadingPanel} from '../section_widgets';
import {
  MEMMAP_GREY,
  SMAPS_CATEGORIES,
  SMAPS_CATEGORY_CASE_SQL,
  SMAPS_FILE_BUCKET_CASE_SQL,
  smapsCategoryColor,
} from '../smaps_categories';

// File-backed subcategories for the memory map's second level, in display
// order. Keys match the buckets produced by SMAPS_FILE_BUCKET_CASE_SQL. All
// share the file-backed colour; they're distinguished by label.
const FILE_BUCKETS = [
  {key: 'so', label: 'Native libs (.so)', color: '#34a853'},
  {key: 'java_code', label: 'Java code (.jar/.oat)', color: '#2e7d46'},
  {key: 'resources', label: 'Resources / APK', color: '#46b966'},
  {key: 'other_file', label: 'Other files', color: '#7cc596'},
];

// Lightens a block colour for its "remainder" sub-block (the part we can't
// attribute to a profiler/reachability figure).
const MEMMAP_LIGHTEN: Record<string, string> = {
  '#4285f4': '#a6c8fa', // native blue → light
  '#f4b400': '#fad470', // java amber → light
};

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
  'Native libs (.so)':
    'Shared libraries (.so) mapped into the process — code and read-only ' +
    'data, mostly clean and shared between processes.',
  'Java code (.jar/.oat)':
    'Compiled and bytecode Java artifacts (.jar/.oat/.odex/.vdex/.art) ' +
    'backing the managed runtime.',
  'Resources / APK':
    'APK archives, fonts and asset files mapped by the app and framework.',
  'Other files': 'File-backed mappings that do not fit the other buckets.',
  'Seen by profiler':
    'Native allocations the heapprofd profiler observed as still unreleased ' +
    'at this snapshot. Allocations made before tracing started are invisible.',
  'Allocator overhead':
    'Native resident memory the profiler could not attribute: allocator ' +
    'metadata, fragmentation and allocations that predate the trace.',
  'Reachable':
    'Java heap still reachable from GC roots at the nearest heap dump — live ' +
    'objects the collector cannot free.',
  'Unreachable / other':
    'Managed heap not accounted for as reachable at the nearest dump: ' +
    'garbage awaiting collection, runtime overhead and dump skew.',
};

// Per-category resident/swap/anon bytes at one snapshot.
interface CategoryBytes {
  rss: number;
  swap: number;
  anon: number;
}

// One smaps snapshot (a single ts): per-category bytes, the file-backed
// breakdown by bucket and the total footprint. x is seconds from trace start.
interface MemSnapshot {
  readonly ts: time;
  readonly x: number;
  readonly total: number;
  readonly byCategory: ReadonlyMap<string, CategoryBytes>;
  readonly fileByBucket: ReadonlyMap<string, number>;
  // MIN(profiler_smaps.id) for this snapshot — the Smaps track event id used to
  // select it on the timeline (mirrors dev.perfetto.Smaps' _smaps_snapshots).
  readonly eventId: number;
}

// Cross-source figures layered onto the memory map's third level.
interface MemExtras {
  readonly nativeSeen: number; // heapprofd unreleased bytes
  readonly javaReachable: number; // reachable Java heap + native registry bytes
}

// The flame-tree for the selected snapshot, built in a slot keyed by the
// selection so it's constructed once per snapshot change rather than on every
// redraw — the chart's hover tooltip drives frequent redraws, and rebuilding
// the whole tree (and its baseline diff) on each one is wasted work. In compare
// mode each block carries its Δ vs the baseline snapshot's tree.
interface MemTreeResult {
  readonly tree: FlamegraphChartSegment;
}

interface MemoryMapData {
  readonly snapshots: MemSnapshot[];
}

const TITLE = 'Where did all the memory go?';

export interface MemoryMapAttrs {
  readonly trace: Trace;
  readonly upid: number;
  // The page-wide selection (by ts). Undefined → latest snapshot.
  readonly selTs?: time;
  readonly baseTs?: time;
}

export class MemoryMap implements m.ClassComponent<MemoryMapAttrs> {
  // Per-snapshot tree inputs (keyed by upid; the selection doesn't reload it).
  private readonly slot = new QuerySlot<MemoryMapData>();
  // The flame-tree for the selected/baseline snapshot, keyed by selection so it
  // is only (re)built when the snapshot changes, not on every redraw.
  private readonly treeSlot = new QuerySlot<MemTreeResult>();

  onremove() {
    this.slot.dispose();
    this.treeSlot.dispose();
  }

  view({attrs}: m.Vnode<MemoryMapAttrs>): m.Children {
    const {trace, upid, selTs, baseTs} = attrs;
    const subtitle =
      'A complete breakdown of resident memory, from smaps, for the ' +
      'selected snapshot.';
    const data = this.slot.use({
      key: {traceId: trace.traceInfo.uuid, upid},
      queryFn: () => loadMemoryMapData(trace, upid),
    }).data;
    if (data === undefined) {
      return loadingPanel({title: TITLE, subtitle}); // Still loading.
    }

    const snaps = data.snapshots;
    if (snaps.length === 0) {
      return emptyPanel({
        title: TITLE,
        subtitle,
        message: 'No smaps data in this trace for this process.',
      });
    }

    // Resolve the selection to snapshots. No selection → latest.
    const snap = nearestByTs(snaps, selTs)!;
    const selIdx = snaps.indexOf(snap);
    const baseSnap =
      baseTs !== undefined ? nearestByTs(snaps, baseTs) : undefined;
    const base =
      baseSnap !== undefined && baseSnap.ts !== snap.ts ? baseSnap : undefined;
    const baseIdx = base !== undefined ? snaps.indexOf(base) : undefined;
    const comparing = base !== undefined;

    // The flame-tree for the resolved snapshot, built (with its baseline diff)
    // in a slot keyed by the selection so it's only constructed when the
    // snapshot changes, not on every (hover-driven) redraw.
    const tree = this.treeSlot.use({
      key: {
        traceId: trace.traceInfo.uuid,
        upid,
        sel: snap.ts.toString(),
        base: base !== undefined ? base.ts.toString() : null,
      },
      queryFn: async () => {
        // In compare mode, annotate each block with its Δ vs the baseline tree.
        const baseByLabel =
          base !== undefined
            ? flattenMemTree(
                buildMemTree(base, await loadExtras(trace, upid, base.ts)),
                new Map(),
              )
            : undefined;
        const selExtras = await loadExtras(trace, upid, snap.ts);
        return {tree: buildMemTree(snap, selExtras, baseByLabel)};
      },
    }).data?.tree;

    const first = snaps[0];
    const last = snaps[snaps.length - 1];
    // Insight: biggest slice of the latest snapshot + fastest-growing slice.
    const catVal = (s: MemSnapshot, key: string) => {
      const r = s.byCategory.get(key);
      return r ? r.rss + r.swap : 0;
    };
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

    const smapsTrackNode = findProcessTrack(
      trace,
      upid,
      (k) => k === SMAPS_TRACK_KIND,
    );

    return m(
      Panel,
      m(Panel.Header, {
        title: TITLE,
        subtitle,
        controls: showInTimelineLink(trace, smapsTrackNode?.uri, snap.eventId),
      }),
      m(
        Panel.Body,
        m('.pf-memscope-memmap', [
          m(Callout, {intent: Intent.Primary}, insight),
          m('.pf-memscope-memmap__section-header', [
            m(
              'span.pf-memscope-memmap__section-title',
              {
                className:
                  comparing && baseIdx !== undefined
                    ? 'pf-memscope-memmap__section-title--diff'
                    : 'pf-memscope-memmap__section-title--straight',
              },
              comparing && baseIdx !== undefined
                ? `Diffing snapshot #${baseIdx + 1} → #${selIdx + 1}`
                : `Snapshot #${selIdx + 1} — t=${snap.x.toFixed(0)}s`,
            ),
            m(
              'span.pf-memscope-memmap__section-hint',
              comparing
                ? 'block widths from the later snapshot · hover for Δ vs baseline'
                : 'absolute resident memory · hover a block for details',
            ),
          ]),
          tree !== undefined
            ? m(FlamegraphChart, {
                data: tree,
                formatValue: formatBytes,
                hideRoot: true,
              })
            : m('.pf-memscope-placeholder', 'Building memory map…'),
        ]),
      ),
    );
  }
}

async function loadMemoryMapData(
  trace: Trace,
  upid: number,
): Promise<MemoryMapData> {
  // Per-(snapshot, category) resident/swap/anon.
  const cats = new Map<bigint, Map<string, CategoryBytes>>();
  const catRes = await trace.engine.query(`
    SELECT
      s.ts AS ts,
      ${SMAPS_CATEGORY_CASE_SQL} AS category,
      CAST(ifnull(SUM(s.rss_kb), 0) * 1024 AS INT) AS rss,
      CAST(ifnull(SUM(s.swap_kb), 0) * 1024 AS INT) AS swap,
      CAST(ifnull(SUM(s.anonymous_kb), 0) * 1024 AS INT) AS anon
    FROM profiler_smaps s
    WHERE s.upid = ${upid}
    GROUP BY s.ts, category
    ORDER BY s.ts ASC
  `);
  for (
    const it = catRes.iter({
      ts: LONG,
      category: STR,
      rss: NUM,
      swap: NUM,
      anon: NUM,
    });
    it.valid();
    it.next()
  ) {
    let byCat = cats.get(it.ts);
    if (byCat === undefined) {
      byCat = new Map();
      cats.set(it.ts, byCat);
    }
    byCat.set(it.category, {rss: it.rss, swap: it.swap, anon: it.anon});
  }

  // Per-(snapshot, file-bucket) resident+swap for file-backed mappings.
  const files = new Map<bigint, Map<string, number>>();
  const fileRes = await trace.engine.query(`
    SELECT
      s.ts AS ts,
      ${SMAPS_FILE_BUCKET_CASE_SQL} AS bucket,
      CAST(ifnull(SUM(s.rss_kb + s.swap_kb), 0) * 1024 AS INT) AS bytes
    FROM profiler_smaps s
    WHERE s.upid = ${upid} AND s.path GLOB '/*'
    GROUP BY s.ts, bucket
    ORDER BY s.ts ASC
  `);
  for (
    const it = fileRes.iter({ts: LONG, bucket: STR, bytes: NUM});
    it.valid();
    it.next()
  ) {
    let byBucket = files.get(it.ts);
    if (byBucket === undefined) {
      byBucket = new Map();
      files.set(it.ts, byBucket);
    }
    byBucket.set(it.bucket, (byBucket.get(it.bucket) ?? 0) + it.bytes);
  }

  // MIN(object id) per snapshot — the Smaps track event id for that ts
  // (mirrors how dev.perfetto.Smaps builds its _smaps_snapshots events).
  const eventIdByTs = new Map<bigint, number>();
  const eventRes = await trace.engine.query(`
    SELECT ts, MIN(id) AS event_id
    FROM profiler_smaps
    WHERE upid = ${upid}
    GROUP BY ts
  `);
  for (
    const it = eventRes.iter({ts: LONG, event_id: NUM});
    it.valid();
    it.next()
  ) {
    eventIdByTs.set(it.ts, it.event_id);
  }

  const start = trace.traceInfo.start;
  const snapshots: MemSnapshot[] = Array.from(cats.entries()).map(
    ([ts, byCategory]) => {
      let total = 0;
      for (const r of byCategory.values()) total += r.rss + r.swap;
      return {
        ts: Time.fromRaw(ts),
        x: Number(ts - start) / 1e9,
        total,
        byCategory,
        fileByBucket: files.get(ts) ?? new Map(),
        eventId: eventIdByTs.get(ts) ?? -1,
      };
    },
  );

  return {snapshots};
}

// Cross-source figures for the snapshot at `ts`. nativeSeen is the cumulative
// heapprofd unreleased footprint up to ts (summed across heaps); javaReachable
// is the reachable Java heap of the heap-graph dump nearest ts. Both are
// best-effort — absent data yields 0 and the third-level split is skipped.
async function loadExtras(
  trace: Trace,
  upid: number,
  ts: time | undefined,
): Promise<MemExtras> {
  if (ts === undefined) return {nativeSeen: 0, javaReachable: 0};

  let nativeSeen = 0;
  const nativeRes = await trace.engine.query(`
    SELECT CAST(ifnull(SUM(a.size), 0) AS INT) AS seen
    FROM heap_profile_allocation a
    WHERE a.upid = ${upid} AND a.ts <= ${ts}
  `);
  const nativeIt = nativeRes.firstRow({seen: NUM});
  nativeSeen = Math.max(0, nativeIt.seen);

  let javaReachable = 0;
  try {
    await trace.engine.query(
      'INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_stats;',
    );
    const javaRes = await trace.engine.query(`
      SELECT
        CAST(s.reachable_heap_size + s.reachable_native_alloc_registry_size
          AS INT) AS reachable
      FROM android_heap_graph_stats s
      WHERE s.upid = ${upid}
      ORDER BY ABS(s.graph_sample_ts - ${ts}) ASC
      LIMIT 1
    `);
    const javaIt = javaRes.iter({reachable: NUM});
    if (javaIt.valid()) javaReachable = Math.max(0, javaIt.reachable);
  } catch {
    // No heap graph — leave javaReachable at 0.
  }

  return {nativeSeen, javaReachable};
}

// Builds the nested region tree for one snapshot, shaped for FlameChartSimple.
// The root is the resident+swap total; each level splits proportionally:
//   root        → File-backed / Anonymous / Graphics / Other
//   File-backed → .so / .jar-.oat / resources / other
//   Anonymous   → Native / Java / Thread stacks / Other anon
//   Native      → Seen by profiler / Allocator overhead
//   Java        → Reachable / Unreachable / other
// `baseByLabel` (compare mode) adds a "Δ vs baseline" line to each tooltip.
function buildMemTree(
  snap: MemSnapshot,
  extras: MemExtras,
  baseByLabel?: Map<string, number>,
): FlamegraphChartSegment {
  const get = (key: string) => snap.byCategory.get(key);
  const resSwap = (r?: CategoryBytes) => (r ? r.rss + r.swap : 0);

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
  const fileBytes = resSwap(file);
  const nativeBytes = resSwap(native);
  const javaBytes = resSwap(java);
  const stackBytes = resSwap(stack);
  const graphicsBytes = resSwap(graphics);
  const anonBytes = nativeBytes + javaBytes + stackBytes + otherAnon;
  const total = fileBytes + anonBytes + graphicsBytes + otherNonAnon;

  const nativeColor = smapsCategoryColor('native');
  const javaColor = smapsCategoryColor('java');
  // Clamp the cross-source figures to the smaps block size so a sub-block
  // never overflows its parent.
  const nativeSeen = Math.min(Math.max(0, extras.nativeSeen), nativeBytes);
  const javaReachable = Math.min(Math.max(0, extras.javaReachable), javaBytes);

  // One node, with a tooltip carrying the share and the plain-language
  // explanation (when one is known for the label).
  const seg = (
    label: string,
    bytes: number,
    color: string,
    children: FlamegraphChartSegment[] = [],
  ): FlamegraphChartSegment => {
    const share = total > 0 ? ((bytes / total) * 100).toFixed(1) : '0';
    const tip: m.Children = [
      m('.pf-flamechart-tooltip__name', label),
      m('.pf-flamechart-tooltip__value', `${formatBytes(bytes)} · ${share}%`),
    ];
    const baseBytes = baseByLabel?.get(label);
    if (baseBytes !== undefined) {
      tip.push(
        deltaText(bytes - baseBytes, `Δ ${formatDelta(bytes - baseBytes)}`),
      );
    }
    const info = MEMMAP_BLOCK_INFO[label];
    if (info !== undefined) {
      tip.push(m('.pf-flamechart-tooltip__desc', info));
    }
    return {name: label, value: bytes, cssColor: color, children, tooltip: tip};
  };

  // File-backed second level: split into path buckets, scaled so they sum to
  // exactly fileBytes (the bucket query and the category query round
  // independently). If no bucket data, dump it all into "other files".
  const bucketBytes = FILE_BUCKETS.map(
    (b) => snap.fileByBucket.get(b.key) ?? 0,
  );
  const bucketTotal = bucketBytes.reduce((s, b) => s + b, 0);
  const fileChildren = FILE_BUCKETS.map((b, i) =>
    seg(
      b.label,
      bucketTotal > 0
        ? (bucketBytes[i] / bucketTotal) * fileBytes
        : b.key === 'other_file'
          ? fileBytes
          : 0,
      b.color,
    ),
  ).filter((s) => s.value > 0);

  const anonChildren = [
    seg(
      'Native',
      nativeBytes,
      nativeColor,
      [
        seg('Seen by profiler', nativeSeen, nativeColor),
        seg(
          'Allocator overhead',
          nativeBytes - nativeSeen,
          MEMMAP_LIGHTEN[nativeColor] ?? nativeColor,
        ),
      ].filter((s) => s.value > 0),
    ),
    seg(
      'Java',
      javaBytes,
      javaColor,
      [
        seg('Reachable', javaReachable, javaColor),
        seg(
          'Unreachable / other',
          javaBytes - javaReachable,
          MEMMAP_LIGHTEN[javaColor] ?? javaColor,
        ),
      ].filter((s) => s.value > 0),
    ),
    seg('Thread stacks', stackBytes, smapsCategoryColor('stack')),
    seg('Other anon', otherAnon, smapsCategoryColor('other')),
  ].filter((s) => s.value > 0);

  const children = [
    seg('File-backed', fileBytes, smapsCategoryColor('file'), fileChildren),
    seg('Anonymous', anonBytes, smapsCategoryColor('native'), anonChildren),
    seg('Graphics', graphicsBytes, smapsCategoryColor('graphics')),
    seg('Other', otherNonAnon, smapsCategoryColor('other')),
  ].filter((s) => s.value > 0);

  return seg('Resident + swap', total, MEMMAP_GREY, children);
}

// Flattens a region tree to a label → bytes map, for diffing one snapshot's
// tree against another's (labels are unique across the tree).
function flattenMemTree(
  seg: FlamegraphChartSegment,
  out: Map<string, number>,
): Map<string, number> {
  out.set(seg.name, seg.value);
  for (const c of seg.children) flattenMemTree(c, out);
  return out;
}
