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
import {Gate} from '../../../../base/mithril_utils';
import {QuerySlot} from '../../../../base/query_slot';
import {Time, type time} from '../../../../base/time';
import {ProportionBar} from '../../../../components/widgets/charts/proportion_bar';
import type {Trace} from '../../../../public/trace';
import {
  LONG,
  LONG_NULL,
  NUM,
  STR,
} from '../../../../trace_processor/query_result';
import {Icon} from '../../../../widgets/icon';
import {Inset} from '../../components/inset';
import {Panel} from '../../components/panel';
import {SubPage} from '../../components/page';
import {deltaColor, formatDelta} from './mem_format';
import {type MemSelection, nearestByTs} from './selection';
import {SmapsDetail} from './smaps_detail';
import {SMAPS_CATEGORIES, SMAPS_CATEGORY_CASE_SQL} from './smaps_categories';
import {BitmapsSection} from './summary/bitmaps_section';
import {CompositionTimeline} from './summary/composition_timeline';
import {JavaSection} from './summary/java_section';
import {MemoryMap} from './summary/memory_map';
import {NativeSection} from './summary/native_section';
import {TraceOverview} from './summary/trace_overview';
import './landing_page.scss';

// Sample count and observed time span of one capture source (smaps / heapprofd)
// for a process. spanS is undefined when there are fewer than two samples.
interface SourceFacts {
  readonly samples: number;
  readonly spanS?: number;
}

// The terse facts the header capture strip shows for the selected process: its
// name and a per-source summary. Each source is loaded with a cheap aggregate
// query rather than pulling every row.
interface CaptureInfo {
  readonly processName: string;
  readonly smaps: SourceFacts;
  readonly dumps: number;
  readonly native: SourceFacts;
}

// One smaps snapshot's resident+swap memory for one process, split by the full
// smaps category taxonomy. Powers the "where the growth went" bar, which spans
// the whole trace. Uses the same rss+swap metric and category split as the
// composition-over-time chart so the two read consistently.
interface GrowthSnapshot {
  ts: time;
  total: number;
  byCategory: ReadonlyMap<string, number>; // category key → rss+swap bytes
}

// Loads the per-snapshot rss+swap breakdown for one process across the whole
// trace, for the growth bar (same path → category classification as the
// composition chart).
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
}

export class ProcessMemDetails
  implements m.ClassComponent<ProcessMemDetailsAttrs>
{
  // The capture-strip facts: process name + per-source sample counts. Its own
  // slot keyed by (trace, upid) so it reloads when the selected process changes.
  private readonly captureSlot = new QuerySlot<CaptureInfo>();
  // Whole-trace per-snapshot smaps breakdown for the growth bar (keyed by
  // upid; independent of the page's snapshot selection).
  private readonly growthSlot = new QuerySlot<GrowthSnapshot[]>();
  private activeTab: 'summary' | 'smaps' = 'summary';
  // The page-wide snapshot selection, driven by the composition timeline and
  // shared with the other summary sections as they're added.
  private selection?: MemSelection;

  onremove() {
    this.captureSlot.dispose();
    this.growthSlot.dispose();
  }

  view({attrs}: m.Vnode<ProcessMemDetailsAttrs>) {
    const {trace, upid} = attrs;
    let capture: CaptureInfo | undefined;
    let error: string | undefined;
    try {
      capture = this.captureSlot.use({
        key: {traceId: trace.traceInfo.uuid, upid},
        queryFn: () => loadCaptureInfo(trace, upid),
      }).data;
    } catch (e) {
      error = String(e);
    }

    // Both tab bodies stay mounted and are toggled with a Gate (display:none
    // when hidden) rather than conditionally rendered, so switching tabs doesn't
    // remount each tab's components and re-run their data loads.
    return [
      error !== undefined && m('p.pf-error', `Error: ${error}`),
      error === undefined && this.renderCaptureStrip(trace, capture),
      error === undefined && this.renderTabs(),
      error === undefined &&
        m(
          Gate,
          {open: this.activeTab === 'summary'},
          // SubPage gives its direct children the cascading fade-up entrance
          // animation (.pf-memscope-subpage > *), matching the rest of the page.
          m(
            SubPage,
            m(TraceOverview, {trace, upid}),
            m(CompositionTimeline, {
              trace,
              upid,
              selection: this.selection,
              onSelect: (s: MemSelection) => (this.selection = s),
              belowChart: this.renderGrowthBar(
                trace,
                upid,
                this.selection?.sel,
                this.selection?.base,
              ),
            }),
            m(MemoryMap, {
              trace,
              upid,
              selTs: this.selection?.sel,
              baseTs: this.selection?.base,
            }),
            m(JavaSection, {
              trace,
              upid,
              selTs: this.selection?.sel,
              baseTs: this.selection?.base,
            }),
            m(BitmapsSection, {
              trace,
              upid,
              selTs: this.selection?.sel,
              baseTs: this.selection?.base,
            }),
            m(NativeSection, {
              trace,
              upid,
              selTs: this.selection?.sel,
              baseTs: this.selection?.base,
            }),
          ),
        ),
      error === undefined &&
        m(
          Gate,
          {open: this.activeTab === 'smaps'},
          m(SubPage, m(SmapsDetail, {trace, upid})),
        ),
    ];
  }

  private renderTabs(): m.Children {
    const tabs: {key: 'summary' | 'smaps'; label: string; icon: string}[] = [
      {key: 'summary', label: 'Summary', icon: 'description'},
      {key: 'smaps', label: 'Smaps Detail', icon: 'table_rows'},
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

  // "Where the growth went" bar (rendered below the composition chart): the
  // per-category memory delta between the baseline and selected snapshots (or
  // over the whole trace when unselected). Undefined while loading, or when
  // there aren't two snapshots to diff (the composition panel covers that case).
  private renderGrowthBar(
    trace: Trace,
    upid: number,
    selTs?: time,
    baseTs?: time,
  ): m.Children {
    const snaps = this.growthSlot.use({
      key: {traceId: trace.traceInfo.uuid, upid},
      queryFn: () => loadGrowthData(trace, upid),
    }).data;
    if (snaps === undefined || snaps.length < 2) return undefined;
    const firstSnap = snaps[0];
    const lastSnap = snaps[snaps.length - 1];
    // Selected endpoint (nearest snapshot), or the latest when unselected.
    const sel = selTs !== undefined ? nearestByTs(snaps, selTs)! : lastSnap;
    // Baseline endpoint: an explicit baseline (when diffing), otherwise the
    // first snapshot so a single selection reads as "growth up to here".
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
    // Growth rate, normalised to bytes/hour over the observed snapshot span.
    const spanSeconds = Number(last.ts - first.ts) / 1e9;
    const ratePerHour =
      spanSeconds > 0 ? (total / spanSeconds) * 3600 : undefined;

    return m(Inset, {className: 'pf-memscope-growth'}, [
      m(
        '.pf-memscope-growth__label',
        shrinking ? 'Where the shrinkage went' : 'Where the growth went',
      ),
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
            `· ${formatDelta(ratePerHour)}/hour`,
          ),
      ]),
      m(ProportionBar, {
        segments: categories.map((c) => ({
          label: c.name,
          // Only regions moving in the dominant direction size the bar; the
          // rest get zero width but still appear in the legend.
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

  // Header capture strip: trace identity plus one colored dot + terse facts
  // per source (smaps / java_hprof / heapprofd), for the selected process. The
  // trace title/duration are known immediately; `info` is undefined while its
  // query is in flight, in which case the per-process facts render as "…" so
  // the strip is laid out from first paint rather than popping in.
  private renderCaptureStrip(trace: Trace, info?: CaptureInfo): m.Children {
    const durationS = Number(trace.traceInfo.end - trace.traceInfo.start) / 1e9;
    const loading = info === undefined;

    const sourceFacts = (f: SourceFacts): string | undefined =>
      f.samples > 0
        ? `${f.samples} samples` +
          (f.spanS !== undefined ? ` over ${f.spanS.toFixed(1)}s` : '')
        : undefined;

    const sources: {key: string; label: string; facts?: string}[] = [
      {key: 'smaps', label: 'smaps', facts: info && sourceFacts(info.smaps)},
      {
        key: 'dump',
        label: 'java_hprof',
        facts: info && (info.dumps > 0 ? `${info.dumps} dumps` : undefined),
      },
      {
        key: 'native',
        label: 'heapprofd',
        facts: info && sourceFacts(info.native),
      },
    ];

    return m(Panel, {className: 'pf-memscope-capture'}, [
      m('.pf-memscope-capture__identity', [
        m('span.pf-memscope-capture__process', info?.processName ?? '…'),
        m('span', `${trace.traceInfo.traceTitle} · ${durationS.toFixed(1)}s`),
      ]),
      m(
        '.pf-memscope-capture__sources',
        sources.map((s) =>
          m(
            'span.pf-memscope-capture__source',
            {
              // Only dim a source once we know it has no data; while loading
              // we don't yet know, so leave it at full strength.
              className:
                !loading && s.facts === undefined
                  ? 'pf-memscope-capture__source--empty'
                  : undefined,
            },
            [
              m(
                `span.pf-memscope-capture__dot.pf-memscope-capture__dot--${s.key}`,
              ),
              m('span.pf-memscope-capture__label', s.label),
              m(
                'span.pf-memscope-capture__facts',
                loading ? '…' : s.facts ?? 'none',
              ),
            ],
          ),
        ),
      ),
    ]);
  }
}

// Runs a `(n, min_ts, max_ts)` aggregate and turns it into a SourceFacts: the
// distinct-sample count and, when there's more than one, the wall-clock span
// between the first and last sample.
async function loadSourceFacts(
  trace: Trace,
  sql: string,
): Promise<SourceFacts> {
  const res = await trace.engine.query(sql);
  const it = res.iter({n: NUM, min_ts: LONG_NULL, max_ts: LONG_NULL});
  if (!it.valid() || it.n === 0) return {samples: 0};
  const spanS =
    it.n > 1 && it.min_ts !== null && it.max_ts !== null
      ? Number(it.max_ts - it.min_ts) / 1e9
      : undefined;
  return {samples: it.n, spanS};
}

async function loadCaptureInfo(
  trace: Trace,
  upid: number,
): Promise<CaptureInfo> {
  const nameRes = await trace.engine.query(`
    SELECT coalesce(name, '<unknown>') AS pname FROM process WHERE upid = ${upid}
  `);
  const nameIt = nameRes.iter({pname: STR});
  const processName = nameIt.valid() ? nameIt.pname : '<unknown>';

  const smaps = await loadSourceFacts(
    trace,
    `SELECT COUNT(DISTINCT ts) AS n, MIN(ts) AS min_ts, MAX(ts) AS max_ts
     FROM profiler_smaps WHERE upid = ${upid}`,
  );

  const dumpRes = await trace.engine.query(`
    SELECT COUNT(*) AS n FROM heap_profile_events
    WHERE upid = ${upid} AND type = 'java_heap_graph'
  `);
  const dumpIt = dumpRes.iter({n: NUM});
  const dumps = dumpIt.valid() ? dumpIt.n : 0;

  const native = await loadSourceFacts(
    trace,
    `SELECT COUNT(DISTINCT ts) AS n, MIN(ts) AS min_ts, MAX(ts) AS max_ts
     FROM heap_profile_allocation WHERE upid = ${upid}`,
  );

  return {processName, smaps, dumps, native};
}
