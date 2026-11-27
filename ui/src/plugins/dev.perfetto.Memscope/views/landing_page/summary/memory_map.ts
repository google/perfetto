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

// The "Memory Breakdown" panel: a complete breakdown of resident memory for one
// process, from smaps, rendered as a multi-level interactive SVG sunburst.
// Drills down into:
//   - File-backed → bucket → top mapped files (smaps paths)
//   - Native → Seen by profiler → top unreleased callsites (heapprofd)
//   - Java → Reachable → either a multi-level dominator tree (disjoint retained
//     sizes) or top classes by shallow + registered native size.

import m from 'mithril';
import {AsyncMemo} from '../../../../../base/async_memo';
import {Time, type time} from '../../../../../base/time';
import {
  SunburstSvg,
  type SunburstSegment,
} from '../../../../../components/widgets/charts_svg/sunburst_svg';
import type {Trace} from '../../../../../public/trace';
import {SMAPS_TRACK_KIND} from '../../../../../public/track_kinds';
import {
  LONG,
  NUM,
  STR,
  STR_NULL,
} from '../../../../../trace_processor/query_result';
import {
  Button,
  ButtonGroup,
  ButtonVariant,
} from '../../../../../widgets/button';
import {Intent} from '../../../../../widgets/common';
import {Callout} from '../../../components/callout';
import {Panel} from '../../../components/panel';
import {deltaText, formatBytes, formatDelta} from '../mem_format';
import {findProcessTrack, showInTimelineLink} from '../process_links';
import {emptyPanel, loadingPanel, shortClassName} from '../section_widgets';
import {nearestByTs} from '../selection';
import {
  MEMMAP_GREY,
  SMAPS_CATEGORIES,
  SMAPS_CATEGORY_CASE_SQL,
  SMAPS_FILE_BUCKET_CASE_SQL,
  smapsCategoryColor,
} from '../smaps_categories';
import {categorizeFrame, extractProcessToken} from './frame_categories';

const TOP_ITEMS = 8;
const TOP_DOM_CHILDREN = 6;
const TOP_FILE_PATHS = 8;

type JavaSunburstMode = 'dominators' | 'shallow';

// File-backed subcategories for the memory map's second level, in display
// order. Keys match the buckets produced by SMAPS_FILE_BUCKET_CASE_SQL.
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

// Harmonious family palettes for outer drill-down rings.
const NATIVE_PALETTE = [
  '#1a73e8',
  '#2b7de9',
  '#3c89ec',
  '#4f96ef',
  '#63a4f2',
  '#77b2f5',
  '#8cc0f8',
  '#5c6bc0',
];
const NATIVE_OTHER_COLOR = '#b8d4fc';

const JAVA_PALETTE = [
  '#e09f00',
  '#eba900',
  '#f4b400',
  '#f7c126',
  '#f9cc4d',
  '#e67e22',
  '#d9822b',
  '#c9961a',
];
const JAVA_SELF_COLOR = '#fbe08c';
const JAVA_OTHER_COLOR = '#f7e5b0';

const FILE_PALETTE = [
  '#1e8e3e',
  '#289a47',
  '#34a853',
  '#45b563',
  '#57c274',
  '#6ccf86',
  '#2e7d46',
  '#4ca368',
];
const FILE_OTHER_COLOR = '#a8dab5';

// Plain-language explanation shown under the block breakdown while hovering
// each top-level block of the memory map.
const MEMMAP_BLOCK_INFO: Record<string, string> = {
  'File-backed':
    'Memory backed by files on disk: code (.so/.oat), fonts and resources. ' +
    'Mostly clean - the kernel can drop and re-read it under pressure.',
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
    'Dalvik/ART managed heap and runtime spaces - everything the Java ' +
    'garbage collector manages.',
  'Thread stacks': 'Stack memory for every live thread in the process.',
  'Other anon':
    'Anonymous memory not attributable to a specific allocator: plain ' +
    'mmap() regions, untagged buffers, IPC shared memory.',
  'Native libs (.so)':
    'Shared libraries (.so) mapped into the process - code and read-only ' +
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
    'Java heap still reachable from GC roots at the nearest heap dump - live ' +
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
  // MIN(profiler_smaps.id) for this snapshot - the Smaps track event id used to
  // select it on the timeline (mirrors dev.perfetto.Smaps' _smaps_snapshots).
  readonly eventId: number;
}

interface FilePathItem {
  readonly path: string;
  readonly bytes: number;
}

interface NativeCallsiteItem {
  readonly callsiteId: number;
  readonly label: string;
  readonly bytes: number;
  readonly allocs: number;
  readonly frames: readonly string[];
}

interface JavaShallowClassItem {
  readonly typeName: string;
  readonly shallowBytes: number;
  readonly nativeBytes: number;
  readonly retainedBytes: number;
  readonly objCount: number;
}

interface JavaDomNode {
  readonly cls: string;
  readonly bytes: number;
  readonly selfBytes: number;
  readonly domCount: number;
  readonly instCount: number;
  readonly children: JavaDomNode[];
}

// Cross-source figures and drill-down slices layered onto the memory map.
interface MemExtras {
  readonly nativeSeen: number; // heapprofd unreleased bytes
  readonly nativeCallsites: readonly NativeCallsiteItem[];
  readonly javaReachable: number; // reachable Java heap + native registry bytes
  readonly javaDominators: readonly JavaDomNode[];
  readonly javaShallowClasses: readonly JavaShallowClassItem[];
  readonly filePathsByBucket: ReadonlyMap<string, readonly FilePathItem[]>;
}

interface ExtrasPair {
  readonly sel: MemExtras;
  readonly base?: MemExtras;
}

interface MemoryMapData {
  readonly snapshots: MemSnapshot[];
}

const TITLE = 'Memory Breakdown';

export interface MemoryMapAttrs {
  readonly trace: Trace;
  readonly upid: number;
  // The page-wide selection (by ts). Undefined → latest snapshot.
  readonly selTs?: time;
  readonly baseTs?: time;
  // Scroll smoothly to the Smaps VMAs section below.
  readonly onInspectSmaps?: () => void;
}

export class MemoryMap implements m.ClassComponent<MemoryMapAttrs> {
  // Per-snapshot tree inputs (keyed by upid; the selection doesn't reload it).
  private readonly slot = new AsyncMemo<MemoryMapData>();
  // Cross-source drill-down data for the selected/baseline snapshot.
  private readonly extrasSlot = new AsyncMemo<ExtrasPair>();
  private javaMode: JavaSunburstMode = 'dominators';
  // Cached built tree so hover-driven redraws don't rebuild the hierarchy.
  private cachedExtrasPair?: ExtrasPair;
  private cachedJavaMode?: JavaSunburstMode;
  private cachedSnapTs?: time;
  private cachedBaseTs?: time;
  private cachedTree?: SunburstSegment;

  onremove() {
    this.slot.dispose();
    this.extrasSlot.dispose();
  }

  view({attrs}: m.Vnode<MemoryMapAttrs>): m.Children {
    const {trace, upid, selTs, baseTs} = attrs;
    const data = this.slot.use({
      key: {traceId: trace.traceInfo.uuid, upid},
      compute: () => loadMemoryMapData(trace, upid),
    }).data;
    if (data === undefined) {
      return loadingPanel({title: TITLE}); // Still loading.
    }

    const snaps = data.snapshots;
    if (snaps.length === 0) {
      return emptyPanel({
        title: TITLE,
        message: 'No smaps data in this trace for this process.',
      });
    }

    // Resolve the selection to snapshots. No selection → latest.
    const snap = nearestByTs(snaps, selTs);
    if (snap === undefined) {
      return emptyPanel({
        title: TITLE,
        message: 'No smaps data in this trace for this process.',
      });
    }
    const selIdx = snaps.indexOf(snap);
    const baseSnap =
      baseTs !== undefined ? nearestByTs(snaps, baseTs) : undefined;
    const base =
      baseSnap !== undefined && baseSnap.ts !== snap.ts ? baseSnap : undefined;
    const baseIdx = base !== undefined ? snaps.indexOf(base) : undefined;
    const comparing = base !== undefined;

    const extrasPair = this.extrasSlot.use({
      key: {
        traceId: trace.traceInfo.uuid,
        upid,
        sel: snap.ts.toString(),
        base: base !== undefined ? base.ts.toString() : null,
      },
      compute: async () => {
        const sel = await loadExtras(trace, upid, snap.ts);
        const baseExtras =
          base !== undefined
            ? await loadExtras(trace, upid, base.ts)
            : undefined;
        return {sel, base: baseExtras};
      },
      retainOn: ['sel', 'base'],
    }).data;

    let tree: SunburstSegment | undefined;
    if (extrasPair !== undefined) {
      if (
        this.cachedExtrasPair !== extrasPair ||
        this.cachedJavaMode !== this.javaMode ||
        this.cachedSnapTs !== snap.ts ||
        this.cachedBaseTs !== base?.ts ||
        this.cachedTree === undefined
      ) {
        const baseByPath =
          base !== undefined && extrasPair.base !== undefined
            ? flattenMemTree(
                buildMemTree(base, extrasPair.base, this.javaMode),
                '',
                new Map(),
              )
            : undefined;
        this.cachedTree = buildMemTree(
          snap,
          extrasPair.sel,
          this.javaMode,
          baseByPath,
        );
        this.cachedExtrasPair = extrasPair;
        this.cachedJavaMode = this.javaMode;
        this.cachedSnapTs = snap.ts;
        this.cachedBaseTs = base?.ts;
      }
      tree = this.cachedTree;
    }

    const hasJavaDrilldown =
      extrasPair !== undefined &&
      (extrasPair.sel.javaDominators.length > 0 ||
        extrasPair.sel.javaShallowClasses.length > 0);

    const first = snaps[0];
    const last = snaps[snaps.length - 1];
    // Insight: biggest slice of the latest snapshot + fastest-growing slice.
    const catVal = (s: MemSnapshot, key: string) => {
      const r = s.byCategory.get(key);
      return r ? r.rss + r.swap : 0;
    };
    const stats = SMAPS_CATEGORIES.map((c) => ({
      ...c,
      last: last !== undefined ? catVal(last, c.key) : 0,
      delta:
        first !== undefined && last !== undefined
          ? catVal(last, c.key) - catVal(first, c.key)
          : 0,
    }));
    const biggest = stats.reduce((a, b) => (b.last > a.last ? b : a));
    const fastest = stats.reduce((a, b) => (b.delta > a.delta ? b : a));
    const pct =
      last !== undefined && last.total > 0
        ? Math.round((biggest.last / last.total) * 100)
        : 0;
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
        controls: [
          attrs.onInspectSmaps !== undefined &&
            m(Button, {
              label: 'Jump to VMAs',
              icon: 'table_rows',
              variant: ButtonVariant.Outlined,
              compact: true,
              onclick: attrs.onInspectSmaps,
            }),
          showInTimelineLink(trace, smapsTrackNode?.uri, snap.eventId),
        ],
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
                : `Snapshot #${selIdx + 1} - t=${snap.x.toFixed(0)}s`,
            ),
            m(
              'span.pf-memscope-memmap__section-hint',
              comparing
                ? 'click any ring or legend row to drill down - hover for Δ vs baseline'
                : 'click any ring or legend row to drill down into files, callsites, or Java heap',
            ),
            hasJavaDrilldown &&
              m('.pf-memscope-memmap__java-mode', [
                m('span.pf-memscope-memmap__java-mode-label', 'Java:'),
                m(ButtonGroup, [
                  m(Button, {
                    label: 'Dominators (retained)',
                    compact: true,
                    variant: ButtonVariant.Outlined,
                    active: this.javaMode === 'dominators',
                    tooltip:
                      'Multi-level dominator tree (disjoint retained sizes summing to 100% of reachable Java heap)',
                    onclick: () => {
                      this.javaMode = 'dominators';
                    },
                  }),
                  m(Button, {
                    label: 'Shallow by class',
                    compact: true,
                    variant: ButtonVariant.Outlined,
                    active: this.javaMode === 'shallow',
                    tooltip:
                      'Top classes by shallow + registered native size (exact 100% partition; retained size shown on hover)',
                    onclick: () => {
                      this.javaMode = 'shallow';
                    },
                  }),
                ]),
              ]),
          ]),
          tree !== undefined
            ? m(SunburstSvg, {
                data: tree,
                formatValue: formatBytes,
                size: 356,
                maxVisibleDepth: 4,
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

  // MIN(object id) per snapshot - the Smaps track event id for that ts
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

function shortFrameLabel(raw: string): string {
  // Drop C++/Java parameter list to keep arc & legend labels concise.
  const noParams = raw.replace(/\([^)]*\).*$/, '').trim();
  const base = noParams.length > 0 ? noParams : raw;
  // For C++ scoped names (a::b::c::fn), keep the last 2 segments.
  if (base.includes('::')) {
    const parts = base.split('::').filter((p) => p.length > 0);
    if (parts.length > 2) {
      return parts.slice(-2).join('::');
    }
    return base;
  }
  // For dotted Java names (pkg.Cls.method), keep the last 2 segments.
  if (base.includes('.')) {
    const parts = base.split('.').filter((p) => p.length > 0);
    if (parts.length > 2) {
      return parts.slice(-2).join('.');
    }
  }
  return base;
}

function formatJavaLabel(cls: string): string {
  if (cls.startsWith('java.lang.Class<') && cls.endsWith('>')) {
    return `Class<${shortClassName(cls.slice(16, -1))}>`;
  }
  return shortClassName(cls);
}

function shortPathLabel(path: string, dupBasename: boolean): string {
  const parts = path.split('/').filter((p) => p.length > 0);
  if (parts.length === 0) return path;
  const base = parts[parts.length - 1] ?? path;
  if (!dupBasename || parts.length < 2) return base;
  return `${parts[parts.length - 2]}/${base}`;
}

// Loads cross-source totals + drill-down slices for the snapshot at `ts`:
//   - Top file-backed mappings per bucket (from profiler_smaps)
//   - Native heapprofd unreleased total + top callsites (with stack frames)
//   - Nearest Java heap dump reachable total + 3-level dominator tree + top
//     classes by shallow + registered native size.
async function loadExtras(
  trace: Trace,
  upid: number,
  ts: time | undefined,
): Promise<MemExtras> {
  if (ts === undefined) {
    return {
      nativeSeen: 0,
      nativeCallsites: [],
      javaReachable: 0,
      javaDominators: [],
      javaShallowClasses: [],
      filePathsByBucket: new Map(),
    };
  }

  // 1. Top file-backed paths per bucket at this smaps snapshot.
  const filePathsByBucket = new Map<string, FilePathItem[]>();
  const filePathRes = await trace.engine.query(`
    SELECT
      ${SMAPS_FILE_BUCKET_CASE_SQL} AS bucket,
      s.path AS path,
      CAST(ifnull(SUM(s.rss_kb + s.swap_kb), 0) * 1024 AS INT) AS bytes
    FROM profiler_smaps s
    WHERE s.upid = ${upid} AND s.ts = ${ts} AND s.path GLOB '/*'
    GROUP BY bucket, s.path
    HAVING bytes > 0
    ORDER BY bytes DESC
  `);
  for (
    const it = filePathRes.iter({bucket: STR, path: STR, bytes: NUM});
    it.valid();
    it.next()
  ) {
    let list = filePathsByBucket.get(it.bucket);
    if (list === undefined) {
      list = [];
      filePathsByBucket.set(it.bucket, list);
    }
    if (list.length < TOP_FILE_PATHS) {
      list.push({path: it.path, bytes: it.bytes});
    }
  }

  // 2. Native heapprofd unreleased total + top callsites up to ts.
  let nativeSeen = 0;
  const nativeCallsites: NativeCallsiteItem[] = [];
  const nativeRes = await trace.engine.query(`
    SELECT CAST(ifnull(SUM(a.size), 0) AS INT) AS seen
    FROM heap_profile_allocation a
    WHERE a.upid = ${upid} AND a.ts <= ${ts}
  `);
  nativeSeen = Math.max(0, nativeRes.firstRow({seen: NUM}).seen);

  if (nativeSeen > 0) {
    try {
      const procRes = await trace.engine.query(
        `SELECT name FROM process WHERE upid = ${upid}`,
      );
      const procToken = extractProcessToken(
        procRes.maybeFirstRow({name: STR_NULL})?.name ?? undefined,
      );
      await trace.engine.query(
        'INCLUDE PERFETTO MODULE callstacks.stack_profile;',
      );
      const csRes = await trace.engine.query(`
        WITH
        unrel AS (
          SELECT
            a.callsite_id AS callsite_id,
            SUM(a.size) AS unreleased,
            SUM(CASE WHEN a.count > 0 THEN a.count ELSE 0 END) AS allocs
          FROM heap_profile_allocation a
          WHERE a.upid = ${upid} AND a.ts <= ${ts}
          GROUP BY a.callsite_id
          HAVING SUM(a.size) > 0
        ),
        top_cs AS (
          SELECT callsite_id, unreleased, allocs
          FROM unrel
          ORDER BY unreleased DESC
          LIMIT ${TOP_ITEMS}
        ),
        chain(top_id, unreleased, allocs, node_id, depth) AS (
          SELECT u.callsite_id, u.unreleased, u.allocs, f.id, 0
          FROM top_cs u
          JOIN _callstack_spc_forest f USING (callsite_id)
          WHERE f.is_leaf_function_in_callsite_frame
          UNION ALL
          SELECT c.top_id, c.unreleased, c.allocs, f.parent_id, c.depth + 1
          FROM chain c
          JOIN _callstack_spc_forest f ON f.id = c.node_id
          WHERE f.parent_id IS NOT NULL AND c.depth < 128
        )
        SELECT
          c.top_id AS top_id,
          CAST(c.unreleased AS INT) AS unreleased,
          CAST(c.allocs AS INT) AS allocs,
          c.depth AS depth,
          f.name AS frame_name,
          mp.name AS mapping_name
        FROM chain c
        JOIN _callstack_spc_forest f ON f.id = c.node_id
        LEFT JOIN stack_profile_mapping mp ON f.mapping_id = mp.id
        ORDER BY c.unreleased DESC, c.top_id, c.depth ASC
      `);

      interface RawCallsite {
        readonly callsiteId: number;
        readonly bytes: number;
        readonly allocs: number;
        readonly frames: {name: string; category: string}[];
      }
      const rawById = new Map<number, RawCallsite>();
      const rawList: RawCallsite[] = [];
      for (
        const it = csRes.iter({
          top_id: NUM,
          unreleased: NUM,
          allocs: NUM,
          depth: NUM,
          frame_name: STR,
          mapping_name: STR_NULL,
        });
        it.valid();
        it.next()
      ) {
        let entry = rawById.get(it.top_id);
        if (entry === undefined) {
          entry = {
            callsiteId: it.top_id,
            bytes: it.unreleased,
            allocs: it.allocs,
            frames: [],
          };
          rawById.set(it.top_id, entry);
          rawList.push(entry);
        }
        const name = it.frame_name.trim() || 'unknown';
        const mapping = it.mapping_name ?? undefined;
        entry.frames.push({
          name,
          category: categorizeFrame(name, mapping, procToken),
        });
      }

      // Pick the best representative label per callsite (preferring the leaf-
      // most app frame, then system frame, then leaf frame) and disambiguate
      // duplicate labels with their caller.
      const picked = rawList.map((entry) => {
        const nonRuntime = entry.frames.filter((f) => f.category !== 'runtime');
        const preferred =
          nonRuntime.find((f) => f.category === 'app') ??
          nonRuntime[0] ??
          entry.frames[0];
        const primaryIdx =
          preferred !== undefined ? nonRuntime.indexOf(preferred) : -1;
        const caller =
          primaryIdx >= 0 ? nonRuntime[primaryIdx + 1]?.name : undefined;
        const displayFrames = (
          nonRuntime.length > 0 ? nonRuntime : entry.frames
        )
          .slice(0, 6)
          .map((f) => f.name);
        return {
          entry,
          baseLabel: shortFrameLabel(preferred?.name ?? 'unknown'),
          callerLabel:
            caller !== undefined ? shortFrameLabel(caller) : undefined,
          displayFrames,
        };
      });

      const labelCounts = new Map<string, number>();
      for (const p of picked) {
        labelCounts.set(p.baseLabel, (labelCounts.get(p.baseLabel) ?? 0) + 1);
      }

      for (const p of picked) {
        const dup = (labelCounts.get(p.baseLabel) ?? 0) > 1;
        const label =
          dup && p.callerLabel !== undefined && p.callerLabel !== p.baseLabel
            ? `${p.baseLabel} ← ${p.callerLabel}`
            : p.baseLabel;
        nativeCallsites.push({
          callsiteId: p.entry.callsiteId,
          label,
          bytes: p.entry.bytes,
          allocs: p.entry.allocs,
          frames: p.displayFrames,
        });
      }
    } catch {
      // Callstack tables unavailable — keep top-level nativeSeen only.
    }
  }

  // 3. Nearest Java heap dump reachable total + shallow classes + 3-level
  // dominator tree (disjoint retained-size hierarchy).
  let javaReachable = 0;
  const javaShallowClasses: JavaShallowClassItem[] = [];
  const javaDominators: JavaDomNode[] = [];
  try {
    await trace.engine.query(
      'INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_stats;',
    );
    const javaRes = await trace.engine.query(`
      SELECT
        s.graph_sample_ts AS dump_ts,
        CAST(s.reachable_heap_size + s.reachable_native_alloc_registry_size
          AS INT) AS reachable
      FROM android_heap_graph_stats s
      WHERE s.upid = ${upid}
      ORDER BY ABS(s.graph_sample_ts - ${ts}) ASC
      LIMIT 1
    `);
    const javaIt = javaRes.iter({dump_ts: LONG, reachable: NUM});
    if (javaIt.valid()) {
      const dumpTs = javaIt.dump_ts;
      javaReachable = Math.max(0, javaIt.reachable);

      if (javaReachable > 0) {
        // 3a. Top classes by shallow + registered native size (exact partition).
        try {
          await trace.engine.query(
            'INCLUDE PERFETTO MODULE ' +
              'android.memory.heap_graph.heap_graph_class_aggregation;',
          );
          const shallowRes = await trace.engine.query(`
            SELECT
              ifnull(a.type_name, '[unknown]') AS type_name,
              CAST(a.reachable_size_bytes AS INT) AS shallow_bytes,
              CAST(a.reachable_native_size_bytes AS INT) AS native_bytes,
              CAST(
                a.dominated_size_bytes + a.dominated_native_size_bytes AS INT
              ) AS retained_bytes,
              CAST(a.reachable_obj_count AS INT) AS obj_count
            FROM android_heap_graph_class_aggregation a
            WHERE a.upid = ${upid}
              AND a.graph_sample_ts = ${dumpTs}
              AND (a.reachable_size_bytes + a.reachable_native_size_bytes) > 0
            ORDER BY
              (a.reachable_size_bytes + a.reachable_native_size_bytes) DESC
            LIMIT ${TOP_ITEMS}
          `);
          for (
            const it = shallowRes.iter({
              type_name: STR,
              shallow_bytes: NUM,
              native_bytes: NUM,
              retained_bytes: NUM,
              obj_count: NUM,
            });
            it.valid();
            it.next()
          ) {
            javaShallowClasses.push({
              typeName: it.type_name,
              shallowBytes: Math.max(0, it.shallow_bytes),
              nativeBytes: Math.max(0, it.native_bytes),
              retainedBytes: Math.max(0, it.retained_bytes),
              objCount: Math.max(0, it.obj_count),
            });
          }
        } catch {
          // Class aggregation unavailable.
        }

        // 3b. 3-level Dominator tree grouped by class path (disjoint retained
        // sizes that partition the strongly-reachable Java heap cleanly).
        try {
          await trace.engine.query(
            'INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;',
          );
          const domRes = await trace.engine.query(`
            WITH
            dom AS MATERIALIZED (
              SELECT
                d.id AS id,
                d.idom_id AS idom_id,
                d.depth AS depth,
                ifnull(c.deobfuscated_name, ifnull(c.name, '[unknown]')) AS cls,
                MAX(0, ifnull(d.dominated_size_bytes, 0)) +
                  MAX(0, ifnull(d.dominated_native_size_bytes, 0)) AS dom_bytes,
                MAX(0, ifnull(o.self_size, 0)) +
                  MAX(0, ifnull(o.native_size, 0)) AS self_bytes,
                ifnull(d.dominated_obj_count, 0) AS dom_count
              FROM heap_graph_dominator_tree d
              JOIN heap_graph_object o ON o.id = d.id
              JOIN heap_graph_class c ON c.id = o.type_id
              WHERE o.upid = ${upid}
                AND o.graph_sample_ts = ${dumpTs}
                AND d.depth <= 3
            ),
            d1 AS (
              SELECT id, cls AS cls1, dom_bytes, self_bytes, dom_count
              FROM dom
              WHERE idom_id IS NULL AND depth = 1
            ),
            top_c1 AS (
              SELECT
                cls1,
                SUM(dom_bytes) AS bytes,
                SUM(self_bytes) AS self_bytes,
                SUM(dom_count) AS dom_count,
                COUNT(*) AS inst_count
              FROM d1
              GROUP BY cls1
              HAVING SUM(dom_bytes) > 0
              ORDER BY bytes DESC
              LIMIT ${TOP_ITEMS}
            ),
            d2 AS (
              SELECT
                p.cls1 AS cls1,
                c.id AS id,
                c.cls AS cls2,
                c.dom_bytes AS dom_bytes,
                c.self_bytes AS self_bytes,
                c.dom_count AS dom_count
              FROM d1 p
              JOIN top_c1 tc ON tc.cls1 = p.cls1
              JOIN dom c ON c.idom_id = p.id AND c.depth = 2
            ),
            top_c2 AS (
              SELECT cls1, cls2, bytes, self_bytes, dom_count, inst_count
              FROM (
                SELECT
                  cls1,
                  cls2,
                  SUM(dom_bytes) AS bytes,
                  SUM(self_bytes) AS self_bytes,
                  SUM(dom_count) AS dom_count,
                  COUNT(*) AS inst_count,
                  ROW_NUMBER() OVER (
                    PARTITION BY cls1 ORDER BY SUM(dom_bytes) DESC
                  ) AS rn
                FROM d2
                GROUP BY cls1, cls2
                HAVING SUM(dom_bytes) > 0
              )
              WHERE rn <= ${TOP_DOM_CHILDREN}
            ),
            d3 AS (
              SELECT
                p.cls1 AS cls1,
                p.cls2 AS cls2,
                c.cls AS cls3,
                c.dom_bytes AS dom_bytes,
                c.dom_count AS dom_count
              FROM d2 p
              JOIN top_c2 tc ON tc.cls1 = p.cls1 AND tc.cls2 = p.cls2
              JOIN dom c ON c.idom_id = p.id AND c.depth = 3
            ),
            top_c3 AS (
              SELECT cls1, cls2, cls3, bytes, dom_count, inst_count
              FROM (
                SELECT
                  cls1,
                  cls2,
                  cls3,
                  SUM(dom_bytes) AS bytes,
                  SUM(dom_count) AS dom_count,
                  COUNT(*) AS inst_count,
                  ROW_NUMBER() OVER (
                    PARTITION BY cls1, cls2 ORDER BY SUM(dom_bytes) DESC
                  ) AS rn
                FROM d3
                GROUP BY cls1, cls2, cls3
                HAVING SUM(dom_bytes) > 0
              )
              WHERE rn <= ${TOP_DOM_CHILDREN}
            )
            SELECT
              1 AS lvl,
              cls1,
              '' AS cls2,
              '' AS cls3,
              CAST(bytes AS INT) AS bytes,
              CAST(self_bytes AS INT) AS self_bytes,
              CAST(dom_count AS INT) AS dom_count,
              CAST(inst_count AS INT) AS inst_count
            FROM top_c1
            UNION ALL
            SELECT
              2 AS lvl,
              cls1,
              cls2,
              '' AS cls3,
              CAST(bytes AS INT) AS bytes,
              CAST(self_bytes AS INT) AS self_bytes,
              CAST(dom_count AS INT) AS dom_count,
              CAST(inst_count AS INT) AS inst_count
            FROM top_c2
            UNION ALL
            SELECT
              3 AS lvl,
              cls1,
              cls2,
              cls3,
              CAST(bytes AS INT) AS bytes,
              0 AS self_bytes,
              CAST(dom_count AS INT) AS dom_count,
              CAST(inst_count AS INT) AS inst_count
            FROM top_c3
          `);

          const l1Map = new Map<string, JavaDomNode>();
          const l2Map = new Map<string, JavaDomNode>();
          for (
            const it = domRes.iter({
              lvl: NUM,
              cls1: STR,
              cls2: STR,
              cls3: STR,
              bytes: NUM,
              self_bytes: NUM,
              dom_count: NUM,
              inst_count: NUM,
            });
            it.valid();
            it.next()
          ) {
            if (it.lvl === 1) {
              const node: JavaDomNode = {
                cls: it.cls1,
                bytes: it.bytes,
                selfBytes: it.self_bytes,
                domCount: it.dom_count,
                instCount: it.inst_count,
                children: [],
              };
              l1Map.set(it.cls1, node);
              javaDominators.push(node);
            } else if (it.lvl === 2) {
              const parent = l1Map.get(it.cls1);
              if (parent !== undefined) {
                const node: JavaDomNode = {
                  cls: it.cls2,
                  bytes: it.bytes,
                  selfBytes: it.self_bytes,
                  domCount: it.dom_count,
                  instCount: it.inst_count,
                  children: [],
                };
                parent.children.push(node);
                l2Map.set(`${it.cls1}\u001f${it.cls2}`, node);
              }
            } else if (it.lvl === 3) {
              const parent = l2Map.get(`${it.cls1}\u001f${it.cls2}`);
              if (parent !== undefined) {
                parent.children.push({
                  cls: it.cls3,
                  bytes: it.bytes,
                  selfBytes: 0,
                  domCount: it.dom_count,
                  instCount: it.inst_count,
                  children: [],
                });
              }
            }
          }
        } catch {
          // Dominator tree unavailable — fall back cleanly.
        }
      }
    }
  } catch {
    // No heap graph - leave javaReachable at 0.
  }

  return {
    nativeSeen,
    nativeCallsites,
    javaReachable,
    javaDominators,
    javaShallowClasses,
    filePathsByBucket,
  };
}

// Builds the nested region tree for one snapshot, shaped for SunburstSvg:
//   root        → File-backed / Anonymous / Graphics / Other
//   File-backed → .so / .jar-.oat / resources / other → top mapped paths
//   Anonymous   → Native / Java / Thread stacks / Other anon
//   Native      → Seen by profiler (→ top callsites) / Allocator overhead
//   Java        → Reachable (→ dominator tree OR top shallow classes) /
//                 Unreachable / other
// `baseByPath` (compare mode) adds a "Δ vs baseline" line to each tooltip.
function buildMemTree(
  snap: MemSnapshot,
  extras: MemExtras,
  javaMode: JavaSunburstMode,
  baseByPath?: ReadonlyMap<string, number>,
): SunburstSegment {
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

  const seg = (opts: {
    readonly path: string;
    readonly key?: string;
    readonly label: string;
    readonly tooltipTitle?: string;
    readonly bytes: number;
    readonly color: string;
    readonly children?: SunburstSegment[];
    readonly extraTip?: m.Children;
  }): SunburstSegment => {
    const share = total > 0 ? ((opts.bytes / total) * 100).toFixed(1) : '0';
    const tip: m.Children = [
      m('.pf-sunburst-tooltip__name', opts.tooltipTitle ?? opts.label),
      m(
        '.pf-sunburst-tooltip__value',
        `${formatBytes(opts.bytes)} - ${share}% of footprint`,
      ),
    ];
    const baseBytes = baseByPath?.get(opts.path);
    const delta =
      baseBytes !== undefined ? deltaText(opts.bytes - baseBytes) : undefined;
    if (baseBytes !== undefined) {
      tip.push(
        deltaText(
          opts.bytes - baseBytes,
          `Δ ${formatDelta(opts.bytes - baseBytes)}`,
        ),
      );
    }
    if (opts.extraTip !== undefined) {
      tip.push(opts.extraTip);
    }
    const info = MEMMAP_BLOCK_INFO[opts.label];
    if (info !== undefined && opts.extraTip === undefined) {
      tip.push(m('.pf-sunburst-tooltip__desc', info));
    }
    return {
      key: opts.key ?? opts.label,
      name: opts.label,
      value: opts.bytes,
      cssColor: opts.color,
      children: opts.children ?? [],
      delta,
      tooltip: tip,
    };
  };

  // File-backed level 2 (buckets) + level 3 (top mapped paths per bucket).
  const bucketBytes = FILE_BUCKETS.map(
    (b) => snap.fileByBucket.get(b.key) ?? 0,
  );
  const bucketTotal = bucketBytes.reduce((s, b) => s + b, 0);
  const fileChildren = FILE_BUCKETS.map((b, i) => {
    const targetBytes =
      bucketTotal > 0
        ? ((bucketBytes[i] ?? 0) / bucketTotal) * fileBytes
        : b.key === 'other_file'
          ? fileBytes
          : 0;
    const bucketPath = `File-backed/${b.label}`;
    const rawPaths = extras.filePathsByBucket.get(b.key) ?? [];
    const rawSum = rawPaths.reduce((s, p) => s + p.bytes, 0);
    const scale = rawSum > targetBytes && rawSum > 0 ? targetBytes / rawSum : 1;

    const baseCounts = new Map<string, number>();
    for (const p of rawPaths) {
      const base = p.path.split('/').pop() ?? p.path;
      baseCounts.set(base, (baseCounts.get(base) ?? 0) + 1);
    }

    const pathSegs: SunburstSegment[] = [];
    let usedBytes = 0;
    for (let j = 0; j < rawPaths.length; j++) {
      const item = rawPaths[j];
      if (item === undefined || item.bytes <= 0) continue;
      const scaled = item.bytes * scale;
      usedBytes += scaled;
      const base = item.path.split('/').pop() ?? item.path;
      const label = shortPathLabel(item.path, (baseCounts.get(base) ?? 0) > 1);
      pathSegs.push(
        seg({
          path: `${bucketPath}/${item.path}`,
          key: item.path,
          label,
          tooltipTitle: item.path,
          bytes: scaled,
          color: FILE_PALETTE[j % FILE_PALETTE.length] ?? b.color,
        }),
      );
    }
    const rem = targetBytes - usedBytes;
    if (pathSegs.length > 0 && rem > 1) {
      pathSegs.push(
        seg({
          path: `${bucketPath}/Other files`,
          label: 'Other files',
          bytes: rem,
          color: FILE_OTHER_COLOR,
        }),
      );
    }

    return seg({
      path: bucketPath,
      label: b.label,
      bytes: targetBytes,
      color: b.color,
      children: pathSegs,
    });
  }).filter((s) => s.value > 0);

  // Native level 4: top unreleased heapprofd callsites under "Seen by profiler".
  const nativeSeenPath = 'Anonymous/Native/Seen by profiler';
  const nativeCallsiteSegs: SunburstSegment[] = [];
  if (nativeSeen > 0 && extras.nativeCallsites.length > 0) {
    const rawSum = extras.nativeCallsites.reduce((s, c) => s + c.bytes, 0);
    const scale = rawSum > nativeSeen && rawSum > 0 ? nativeSeen / rawSum : 1;
    let used = 0;
    for (let i = 0; i < extras.nativeCallsites.length; i++) {
      const cs = extras.nativeCallsites[i];
      if (cs === undefined || cs.bytes <= 0) continue;
      const scaled = cs.bytes * scale;
      used += scaled;
      const pctProfiled =
        extras.nativeSeen > 0
          ? ((cs.bytes / extras.nativeSeen) * 100).toFixed(1)
          : '0';
      const extraTip: m.Children = [
        m(
          '.pf-sunburst-tooltip__meta',
          `${pctProfiled}% of profiled native · ${cs.allocs.toLocaleString()} allocs`,
        ),
        cs.frames.length > 0 &&
          m(
            '.pf-sunburst-tooltip__stack',
            cs.frames.map((f, fi) =>
              m('div', {key: fi}, `${fi === 0 ? '' : '↳ '}${f}`),
            ),
          ),
      ];
      nativeCallsiteSegs.push(
        seg({
          path: `${nativeSeenPath}/cs:${cs.callsiteId}`,
          key: `cs:${cs.callsiteId}`,
          label: cs.label,
          tooltipTitle: cs.frames[0] ?? cs.label,
          bytes: scaled,
          color: NATIVE_PALETTE[i % NATIVE_PALETTE.length] ?? nativeColor,
          extraTip,
        }),
      );
    }
    const otherNative = nativeSeen - used;
    if (nativeCallsiteSegs.length > 0 && otherNative > 1) {
      nativeCallsiteSegs.push(
        seg({
          path: `${nativeSeenPath}/Other callsites`,
          label: 'Other callsites',
          bytes: otherNative,
          color: NATIVE_OTHER_COLOR,
          extraTip: m(
            '.pf-sunburst-tooltip__desc',
            'Remaining unreleased native allocations beyond the top callsites.',
          ),
        }),
      );
    }
  }

  // Java level 4(+): either multi-level Dominator tree (disjoint retained
  // sizes) or top classes by shallow + registered native size.
  const javaReachPath = 'Anonymous/Java/Reachable';
  const javaReachChildren: SunburstSegment[] = [];
  const effectiveJavaMode: JavaSunburstMode =
    javaMode === 'dominators' && extras.javaDominators.length > 0
      ? 'dominators'
      : extras.javaShallowClasses.length > 0
        ? 'shallow'
        : 'dominators';

  if (javaReachable > 0 && effectiveJavaMode === 'dominators') {
    const buildDomLevel = (
      nodes: readonly JavaDomNode[],
      parentPath: string,
      parentTargetBytes: number,
      parentSelfBytes: number,
      depth: number,
    ): SunburstSegment[] => {
      if (nodes.length === 0 || parentTargetBytes <= 0) return [];
      const rawChildrenSum = nodes.reduce((s, n) => s + n.bytes, 0);
      const rawTotal = rawChildrenSum + Math.max(0, parentSelfBytes);
      const scale =
        rawTotal > parentTargetBytes && rawTotal > 0
          ? parentTargetBytes / rawTotal
          : 1;
      const out: SunburstSegment[] = [];
      let used = 0;

      for (let i = 0; i < nodes.length; i++) {
        const node = nodes[i];
        if (node === undefined || node.bytes <= 0) continue;
        const scaledBytes = node.bytes * scale;
        const scaledSelf = node.selfBytes * scale;
        used += scaledBytes;
        const nodePath = `${parentPath}/${node.cls}`;
        const pctJava =
          extras.javaReachable > 0
            ? ((node.bytes / extras.javaReachable) * 100).toFixed(1)
            : '0';
        const children =
          node.children.length > 0
            ? buildDomLevel(
                node.children,
                nodePath,
                scaledBytes,
                scaledSelf,
                depth + 1,
              )
            : [];
        const extraTip: m.Children = [
          m(
            '.pf-sunburst-tooltip__meta',
            `Disjoint retained: ${formatBytes(node.bytes)} (${pctJava}% of reachable Java)`,
          ),
          m(
            '.pf-sunburst-tooltip__meta',
            `${node.instCount.toLocaleString()} ${node.instCount === 1 ? 'instance' : 'instances'} dominating ${node.domCount.toLocaleString()} objects` +
              (node.selfBytes > 0
                ? ` · self ${formatBytes(node.selfBytes)}`
                : ''),
          ),
        ];
        out.push(
          seg({
            path: nodePath,
            key: node.cls,
            label: formatJavaLabel(node.cls),
            tooltipTitle: node.cls,
            bytes: scaledBytes,
            color:
              JAVA_PALETTE[(i + (depth - 1) * 2) % JAVA_PALETTE.length] ??
              javaColor,
            children,
            extraTip,
          }),
        );
      }

      // If this parent node retains both children and meaningful shallow bytes
      // of its own, surface "(self)" so child arcs account for 100% cleanly.
      const scaledParentSelf = Math.max(0, parentSelfBytes) * scale;
      if (
        depth > 1 &&
        scaledParentSelf > 1 &&
        scaledParentSelf / parentTargetBytes >= 0.02
      ) {
        used += scaledParentSelf;
        out.push(
          seg({
            path: `${parentPath}/(self)`,
            label: '(self)',
            bytes: scaledParentSelf,
            color: JAVA_SELF_COLOR,
            extraTip: m(
              '.pf-sunburst-tooltip__desc',
              'Shallow + registered native bytes of the parent object(s) themselves.',
            ),
          }),
        );
      }

      const rem = parentTargetBytes - used;
      if (out.length > 0 && rem > 1) {
        const otherLabel = depth === 1 ? 'Other dominators' : 'Other dominated';
        out.push(
          seg({
            path: `${parentPath}/${otherLabel}`,
            label: otherLabel,
            bytes: rem,
            color: JAVA_OTHER_COLOR,
            extraTip: m(
              '.pf-sunburst-tooltip__desc',
              depth === 1
                ? 'Remaining root dominators (and weak-only reachable objects) beyond the top entries.'
                : 'Remaining dominated descendants (plus small self size) beyond the top child classes.',
            ),
          }),
        );
      }
      return out;
    };

    javaReachChildren.push(
      ...buildDomLevel(
        extras.javaDominators,
        javaReachPath,
        javaReachable,
        0,
        1,
      ),
    );
  } else if (javaReachable > 0 && extras.javaShallowClasses.length > 0) {
    const rawSum = extras.javaShallowClasses.reduce(
      (s, c) => s + c.shallowBytes + c.nativeBytes,
      0,
    );
    const scale =
      rawSum > javaReachable && rawSum > 0 ? javaReachable / rawSum : 1;
    let used = 0;
    for (let i = 0; i < extras.javaShallowClasses.length; i++) {
      const cls = extras.javaShallowClasses[i];
      if (cls === undefined) continue;
      const clsBytes = cls.shallowBytes + cls.nativeBytes;
      if (clsBytes <= 0) continue;
      const scaled = clsBytes * scale;
      used += scaled;
      const pctJava =
        extras.javaReachable > 0
          ? ((clsBytes / extras.javaReachable) * 100).toFixed(1)
          : '0';
      const extraTip: m.Children = [
        m(
          '.pf-sunburst-tooltip__meta',
          `${pctJava}% of reachable Java · ${cls.objCount.toLocaleString()} instances`,
        ),
        m(
          '.pf-sunburst-tooltip__meta',
          `Shallow: ${formatBytes(cls.shallowBytes)}` +
            (cls.nativeBytes > 0
              ? ` + Native: ${formatBytes(cls.nativeBytes)}`
              : '') +
            (cls.retainedBytes > 0
              ? ` · Retained: ${formatBytes(cls.retainedBytes)}`
              : ''),
        ),
      ];
      javaReachChildren.push(
        seg({
          path: `${javaReachPath}/${cls.typeName}`,
          key: cls.typeName,
          label: formatJavaLabel(cls.typeName),
          tooltipTitle: cls.typeName,
          bytes: scaled,
          color: JAVA_PALETTE[i % JAVA_PALETTE.length] ?? javaColor,
          extraTip,
        }),
      );
    }
    const otherClasses = javaReachable - used;
    if (javaReachChildren.length > 0 && otherClasses > 1) {
      javaReachChildren.push(
        seg({
          path: `${javaReachPath}/Other classes`,
          label: 'Other classes',
          bytes: otherClasses,
          color: JAVA_OTHER_COLOR,
          extraTip: m(
            '.pf-sunburst-tooltip__desc',
            'Remaining reachable Java classes beyond the top shallow-size entries.',
          ),
        }),
      );
    }
  }

  const anonChildren = [
    seg({
      path: 'Anonymous/Native',
      label: 'Native',
      bytes: nativeBytes,
      color: nativeColor,
      children: [
        seg({
          path: nativeSeenPath,
          label: 'Seen by profiler',
          bytes: nativeSeen,
          color: nativeColor,
          children: nativeCallsiteSegs,
        }),
        seg({
          path: 'Anonymous/Native/Allocator overhead',
          label: 'Allocator overhead',
          bytes: nativeBytes - nativeSeen,
          color: MEMMAP_LIGHTEN[nativeColor] ?? nativeColor,
        }),
      ].filter((s) => s.value > 0),
    }),
    seg({
      path: 'Anonymous/Java',
      label: 'Java',
      bytes: javaBytes,
      color: javaColor,
      children: [
        seg({
          path: javaReachPath,
          label: 'Reachable',
          bytes: javaReachable,
          color: javaColor,
          children: javaReachChildren,
        }),
        seg({
          path: 'Anonymous/Java/Unreachable / other',
          label: 'Unreachable / other',
          bytes: javaBytes - javaReachable,
          color: MEMMAP_LIGHTEN[javaColor] ?? javaColor,
        }),
      ].filter((s) => s.value > 0),
    }),
    seg({
      path: 'Anonymous/Thread stacks',
      label: 'Thread stacks',
      bytes: stackBytes,
      color: smapsCategoryColor('stack'),
    }),
    seg({
      path: 'Anonymous/Other anon',
      label: 'Other anon',
      bytes: otherAnon,
      color: smapsCategoryColor('other'),
    }),
  ].filter((s) => s.value > 0);

  const children = [
    seg({
      path: 'File-backed',
      label: 'File-backed',
      bytes: fileBytes,
      color: smapsCategoryColor('file'),
      children: fileChildren,
    }),
    seg({
      path: 'Anonymous',
      label: 'Anonymous',
      bytes: anonBytes,
      color: smapsCategoryColor('native'),
      children: anonChildren,
    }),
    seg({
      path: 'Graphics',
      label: 'Graphics',
      bytes: graphicsBytes,
      color: smapsCategoryColor('graphics'),
    }),
    seg({
      path: 'Other',
      label: 'Other',
      bytes: otherNonAnon,
      color: smapsCategoryColor('other'),
    }),
  ].filter((s) => s.value > 0);

  return seg({
    path: '',
    label: 'Resident + swap',
    bytes: total,
    color: MEMMAP_GREY,
    children,
  });
}

// Flattens a region tree to a full-path → bytes map for compare-mode diffs.
function flattenMemTree(
  seg: SunburstSegment,
  parentPath: string,
  out: Map<string, number>,
): Map<string, number> {
  const step = seg.key ?? seg.name;
  const path =
    parentPath === ''
      ? ''
      : parentPath === '/'
        ? step
        : `${parentPath}/${step}`;
  out.set(path, seg.value);
  const childPrefix = path === '' ? '/' : path;
  for (const c of seg.children) flattenMemTree(c, childPrefix, out);
  return out;
}
