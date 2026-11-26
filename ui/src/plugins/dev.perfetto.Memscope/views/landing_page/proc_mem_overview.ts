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

import m from 'mithril';
import {AsyncMemo} from '../../../../base/async_memo';
import {Time, type time} from '../../../../base/time';
import {ProportionBar} from '../../../../components/widgets/charts/proportion_bar';
import type {Trace} from '../../../../public/trace';
import {
  LONG,
  LONG_NULL,
  NUM,
  STR,
} from '../../../../trace_processor/query_result';
import type {NavView} from '../../../com.android.HeapDumpExplorer/nav_state';
import {Inset} from '../../components/inset';
import {SubPage} from '../../components/page';
import {deltaColor, formatDelta} from './mem_format';
import {type MemSelection, nearestByTs} from './selection';
import {SMAPS_CATEGORIES, SMAPS_CATEGORY_CASE_SQL} from './smaps_categories';
import {BitmapsSection} from './summary/bitmaps_section';
import {CompositionTimeline} from './summary/composition_timeline';
import {JavaSection} from './summary/java_section';
import {MemoryMap} from './summary/memory_map';
import {NativeSection} from './summary/native_section';
import {TraceOverview} from './summary/trace_overview';

export type MemoryWorkbenchSection = 'overview' | 'smaps' | 'native' | 'heap';

// Sample count and observed time span of one capture source (smaps / heapprofd)
// for a process. spanS is undefined when there are fewer than two samples.
export interface SourceFacts {
  readonly samples: number;
  readonly spanS?: number;
}

export interface FootprintSegment {
  readonly key: string;
  readonly label: string;
  readonly color: string;
  readonly bytes: number;
}

// Per-process capture availability + latest smaps footprint breakdown,
// powering the left Scope Rail process card and section ordering.
export interface CaptureInfo {
  readonly processName: string;
  readonly pid: number;
  readonly smaps: SourceFacts;
  readonly dumps: number;
  readonly hasBitmaps: boolean;
  readonly native: SourceFacts;
  readonly footprintTotal: number;
  readonly footprintSegments: ReadonlyArray<FootprintSegment>;
}

// One smaps snapshot's resident+swap memory for one process, split by the full
// smaps category taxonomy. Powers the "memory delta" bar, which spans
// the whole trace.
interface GrowthSnapshot {
  ts: time;
  total: number;
  byCategory: ReadonlyMap<string, number>;
}

async function loadGrowthData(
  trace: Trace,
  upid: number,
): Promise<GrowthSnapshot[]> {
  const byTs = new Map<
    bigint,
    {ts: time; total: number; byCategory: Map<string, number>}
  >();
  const res = await trace.engine.query(`
    SELECT
      s.ts AS ts,
      ${SMAPS_CATEGORY_CASE_SQL} AS category,
      CAST(ifnull(SUM(s.rss_kb + s.swap_kb), 0) * 1024 AS INT) AS bytes
    FROM profiler_smaps s
    WHERE s.upid = ${upid}
    GROUP BY s.ts, category
    ORDER BY s.ts ASC
  `);
  for (
    const it = res.iter({ts: LONG, category: STR, bytes: NUM});
    it.valid();
    it.next()
  ) {
    let snap = byTs.get(it.ts);
    if (snap === undefined) {
      snap = {ts: Time.fromRaw(it.ts), total: 0, byCategory: new Map()};
      byTs.set(it.ts, snap);
    }
    snap.total += it.bytes;
    snap.byCategory.set(
      it.category,
      (snap.byCategory.get(it.category) ?? 0) + it.bytes,
    );
  }
  return Array.from(byTs.values());
}

export interface ProcessMemDetailsAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly capture?: CaptureInfo;
  readonly selection?: MemSelection;
  readonly onSelect: (selection: MemSelection) => void;
  readonly onInspectSmaps?: () => void;
  readonly onOpenJavaHeap?: (
    ts: time,
    view: NavView,
    params?: Record<string, unknown>,
  ) => void;
}

export class ProcessMemDetails implements m.ClassComponent<ProcessMemDetailsAttrs> {
  private readonly growthSlot = new AsyncMemo<GrowthSnapshot[]>();

  onremove() {
    this.growthSlot.dispose();
  }

  view({attrs}: m.Vnode<ProcessMemDetailsAttrs>) {
    const {
      trace,
      upid,
      capture,
      selection,
      onSelect,
      onInspectSmaps,
      onOpenJavaHeap,
    } = attrs;

    const smapsAvailable = capture === undefined || capture.smaps.samples > 0;
    const timelineAvailable =
      capture === undefined || capture.smaps.samples > 1;
    const dumpsAvailable = capture === undefined || capture.dumps > 0;
    const bitmapsAvailable = capture === undefined || capture.hasBitmaps;
    const nativeAvailable = capture === undefined || capture.native.samples > 0;

    const sections = [
      {
        hasData: timelineAvailable,
        content: m(CompositionTimeline, {
          key: 'composition',
          trace,
          upid,
          selection,
          onSelect,
          belowChart: this.renderGrowthBar(
            trace,
            upid,
            selection?.sel,
            selection?.base,
          ),
        }),
      },
      {
        hasData: smapsAvailable,
        content: m(MemoryMap, {
          key: 'memory-map',
          trace,
          upid,
          selTs: selection?.sel,
          baseTs: selection?.base,
          onInspectSmaps,
        }),
      },
      {
        hasData: dumpsAvailable,
        content: m(JavaSection, {
          key: 'java',
          trace,
          upid,
          selTs: selection?.sel,
          baseTs: selection?.base,
          onOpenJavaHeap,
        }),
      },
      {
        hasData: bitmapsAvailable,
        content: m(BitmapsSection, {
          key: 'bitmaps',
          trace,
          upid,
          selTs: selection?.sel,
          baseTs: selection?.base,
          onOpenBitmapsGallery:
            onOpenJavaHeap !== undefined
              ? (ts: time) => onOpenJavaHeap(ts, 'bitmaps')
              : undefined,
        }),
      },
      {
        hasData: nativeAvailable,
        content: m(NativeSection, {
          key: 'native',
          trace,
          upid,
          selTs: selection?.sel,
          baseTs: selection?.base,
        }),
      },
    ];
    sections.sort((a, b) => Number(b.hasData) - Number(a.hasData));

    return m(
      SubPage,
      m(TraceOverview, {key: 'trace-overview', trace, upid}),
      ...sections.map((section) => section.content),
    );
  }

  private renderGrowthBar(
    trace: Trace,
    upid: number,
    selTs?: time,
    baseTs?: time,
  ): m.Children {
    const snaps = this.growthSlot.use({
      key: {traceId: trace.traceInfo.uuid, upid},
      compute: () => loadGrowthData(trace, upid),
    }).data;
    if (snaps === undefined || snaps.length < 2) return undefined;
    const firstSnap = snaps[0];
    const lastSnap = snaps[snaps.length - 1];
    if (firstSnap === undefined || lastSnap === undefined) return undefined;
    const sel =
      (selTs !== undefined ? nearestByTs(snaps, selTs) : undefined) ?? lastSnap;
    const resolvedBase =
      baseTs !== undefined ? nearestByTs(snaps, baseTs) : undefined;
    const base =
      resolvedBase !== undefined && resolvedBase.ts !== sel.ts
        ? resolvedBase
        : firstSnap;
    const diffing = resolvedBase !== undefined && resolvedBase.ts !== sel.ts;
    const wholeTrace = base.ts === firstSnap.ts && sel.ts === lastSnap.ts;
    const first = base;
    const last = sel;
    const total = last.total - first.total;
    const categories = SMAPS_CATEGORIES.map((c) => ({
      name: c.label,
      color: c.color,
      delta:
        (last.byCategory.get(c.key) ?? 0) - (first.byCategory.get(c.key) ?? 0),
    }));
    const shrinking = total < 0;
    const spanSeconds = Number(last.ts - first.ts) / 1e9;
    const ratePerHour =
      spanSeconds > 0 ? (total / spanSeconds) * 3600 : undefined;

    return m(Inset, {className: 'pf-memscope-growth'}, [
      m('.pf-memscope-growth__label', 'Memory delta'),
      m('.pf-memscope-growth__headline', [
        m(
          'span.pf-memscope-growth__delta',
          {style: {color: deltaColor(total)}},
          formatDelta(total),
        ),
        m(
          'span.pf-memscope-growth__context',
          `${shrinking ? 'removed' : 'added'} ` +
            (diffing
              ? 'vs baseline'
              : wholeTrace
                ? 'over the trace'
                : 'up to selection'),
        ),
        ratePerHour !== undefined &&
          m(
            'span.pf-memscope-growth__context',
            `- ${formatDelta(ratePerHour)}/hour`,
          ),
      ]),
      m(ProportionBar, {
        segments: categories.map((c) => ({
          label: c.name,
          weight: (shrinking ? c.delta < 0 : c.delta > 0)
            ? Math.abs(c.delta)
            : 0,
          color: c.color,
          value: formatDelta(c.delta),
          valueColor: deltaColor(c.delta),
        })),
      }),
    ]);
  }
}

function makeSourceFacts(
  samples: number,
  minTs: bigint | null,
  maxTs: bigint | null,
): SourceFacts {
  if (samples === 0) return {samples: 0};
  const spanS =
    samples > 1 && minTs !== null && maxTs !== null
      ? Number(maxTs - minTs) / 1e9
      : undefined;
  return {samples, spanS};
}

export async function loadCaptureInfo(
  trace: Trace,
  upid: number,
): Promise<CaptureInfo> {
  const res = await trace.engine.query(`
    WITH
    smaps AS (
      SELECT COUNT(DISTINCT ts) AS n, MIN(ts) AS min_ts, MAX(ts) AS max_ts
      FROM profiler_smaps WHERE upid = ${upid}
    ),
    native AS (
      SELECT COUNT(DISTINCT ts) AS n, MIN(ts) AS min_ts, MAX(ts) AS max_ts
      FROM heap_profile_allocation WHERE upid = ${upid}
    )
    SELECT
      coalesce(
        (SELECT COALESCE(cmdline, name) FROM process WHERE upid = ${upid}),
        '<unknown>'
      ) AS pname,
      coalesce((SELECT pid FROM process WHERE upid = ${upid}), 0) AS pid,
      smaps.n AS smaps_n,
      smaps.min_ts AS smaps_min_ts,
      smaps.max_ts AS smaps_max_ts,
      (
        SELECT COUNT(*) FROM heap_graph WHERE upid = ${upid}
      ) AS dumps,
      EXISTS(
        SELECT 1
        FROM heap_graph_object o
        JOIN heap_graph_class c ON c.id = o.type_id
        WHERE o.upid = ${upid} AND o.reachable
          AND (c.name = 'android.graphics.Bitmap'
            OR c.deobfuscated_name = 'android.graphics.Bitmap')
        LIMIT 1
      ) AS has_bitmaps,
      native.n AS native_n,
      native.min_ts AS native_min_ts,
      native.max_ts AS native_max_ts
    FROM smaps, native
  `);
  const row = res.firstRow({
    pname: STR,
    pid: NUM,
    smaps_n: NUM,
    smaps_min_ts: LONG_NULL,
    smaps_max_ts: LONG_NULL,
    dumps: NUM,
    has_bitmaps: NUM,
    native_n: NUM,
    native_min_ts: LONG_NULL,
    native_max_ts: LONG_NULL,
  });

  // Latest smaps snapshot breakdown for the rail's live footprint bar.
  let footprintTotal = 0;
  const byCategory = new Map<string, number>();
  if (row.smaps_max_ts !== null) {
    const fpRes = await trace.engine.query(`
      SELECT
        ${SMAPS_CATEGORY_CASE_SQL} AS category,
        CAST(ifnull(SUM(s.rss_kb + s.swap_kb), 0) * 1024 AS INT) AS bytes
      FROM profiler_smaps s
      WHERE s.upid = ${upid} AND s.ts = ${row.smaps_max_ts}
      GROUP BY category
    `);
    for (
      const it = fpRes.iter({category: STR, bytes: NUM});
      it.valid();
      it.next()
    ) {
      footprintTotal += it.bytes;
      byCategory.set(
        it.category,
        (byCategory.get(it.category) ?? 0) + it.bytes,
      );
    }
  }

  const footprintSegments: FootprintSegment[] = SMAPS_CATEGORIES.map((c) => ({
    key: c.key,
    label: c.label,
    color: c.color,
    bytes: byCategory.get(c.key) ?? 0,
  })).filter((s) => s.bytes > 0);

  return {
    processName: row.pname,
    pid: row.pid,
    smaps: makeSourceFacts(row.smaps_n, row.smaps_min_ts, row.smaps_max_ts),
    dumps: row.dumps,
    hasBitmaps: row.has_bitmaps !== 0,
    native: makeSourceFacts(row.native_n, row.native_min_ts, row.native_max_ts),
    footprintTotal,
    footprintSegments,
  };
}
