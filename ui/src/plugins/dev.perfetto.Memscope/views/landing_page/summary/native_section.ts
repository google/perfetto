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

// "How much native memory did you use, and where did it go?" — heapprofd
// coverage vs the smaps native total, plus the top unreleased call-stacks over
// the whole trace, for one process. Self-contained: owns its query slot and
// loading logic.

import m from 'mithril';
import {AsyncMemo} from '../../../../../base/async_memo';
import {Time, type time} from '../../../../../base/time';
import type {Trace} from '../../../../../public/trace';
import {
  LONG_NULL,
  NUM,
  STR,
  STR_NULL,
} from '../../../../../trace_processor/query_result';
import {Panel} from '../../../components/panel';
import {Callout} from '../../../components/callout';
import {Intent} from '../../../../../widgets/common';
import {deltaText, formatBytes, formatDelta, statCard} from '../mem_format';
import {Ratio} from '../../../components/ratio';
import {ShareBar} from '../../../components/share_bar';
import {findProcessTrack, showAreaInTimelineLink} from '../process_links';
import {emptyPanel, loadingPanel, topTable} from '../section_widgets';
import {SMAPS_CATEGORY_CASE_SQL} from '../smaps_categories';
import {Stack} from '../../../../../widgets/stack';
import {BillboardStrip} from '../../../components/billboard';

const TITLE = 'How much native memory did you use, and where did it go?';
const SUBTITLE =
  "Aggregated by allocation callstack — the clearest signal when it's " +
  'available.';

// One top-unreleased heapprofd callsite. App-owned frames are kept as the
// preferred snippet; when there are none, fallbackFrame is the nearest useful
// framework/library caller above the allocator plumbing.
interface NativeStack {
  unreleased: number;
  allocs: number;
  frames: string[];
  fallbackFrame?: string;
}

interface NativeData {
  readonly hasProfile: boolean;
  // Top app-attributed callsites by unreleased bytes in the selected window.
  readonly stacks: NativeStack[];
  // Total positive-net unreleased bytes in the window (share-bar denominator).
  readonly profileTotal: number;
  // Cumulative unreleased bytes the profiler observed up to the selected
  // snapshot.
  readonly seen: number;
  // In compare mode, the cumulative unreleased up to the baseline snapshot.
  readonly baseSeen?: number;
  // Native allocator footprint (rss+swap) at the selected smaps snapshot.
  readonly nativeRss: number;
  // Thread-stack footprint (rss+swap) at the selected smaps snapshot.
  readonly stackBytes: number;
  readonly threads?: number;
  // First and last native-profile snapshot included in the displayed window.
  // These become the timeline area selection for the aggregated flamegraph.
  readonly rangeStart?: time;
  readonly rangeEnd?: time;
}

// heapprofd data is incremental (per-dump deltas), so a window sum is "what
// was allocated and still unreleased in that window":
//   - single snapshot → window (start, selTs] → cumulative unreleased up to it.
//   - range brushed → window (baseTs, selTs] → net unreleased between the two.
async function loadNativeData(
  trace: Trace,
  upid: number,
  selTs: time | undefined,
  baseTs: time | undefined,
): Promise<NativeData> {
  const countRes = await trace.engine.query(`
    SELECT COUNT(*) AS n FROM heap_profile_allocation WHERE upid = ${upid}
  `);
  const hasProfile = countRes.firstRow({n: NUM}).n > 0;
  if (!hasProfile) {
    return {
      hasProfile: false,
      stacks: [],
      profileTotal: 0,
      seen: 0,
      nativeRss: 0,
      stackBytes: 0,
    };
  }

  // The smaps snapshot to read the native/stack footprint from: the selected
  // one, or the latest when nothing is selected.
  const tsRes = await trace.engine.query(
    `SELECT MAX(ts) AS ts FROM profiler_smaps WHERE upid = ${upid}`,
  );
  const lastSmapsTs = tsRes.firstRow({ts: LONG_NULL}).ts;
  const snapTs: bigint | null = selTs ?? lastSmapsTs;
  let nativeRss = 0;
  let stackBytes = 0;
  if (snapTs !== null) {
    const catRes = await trace.engine.query(`
      SELECT
        ${SMAPS_CATEGORY_CASE_SQL} AS category,
        CAST(ifnull(SUM(s.anonymous_kb + s.swap_kb), 0) * 1024 AS INT) AS bytes
      FROM profiler_smaps s
      WHERE s.upid = ${upid} AND s.ts = ${snapTs}
      GROUP BY category
    `);
    for (
      const it = catRes.iter({category: STR, bytes: NUM});
      it.valid();
      it.next()
    ) {
      if (it.category === 'native') nativeRss = it.bytes;
      else if (it.category === 'stack') stackBytes = it.bytes;
    }
  }

  // Cumulative unreleased up to the selected snapshot (and the baseline, for
  // the compare delta).
  const seen = await cumulativeUnreleased(trace, upid, snapTs);
  const baseSeen =
    baseTs !== undefined
      ? await cumulativeUnreleased(trace, upid, baseTs)
      : undefined;

  const threadsRes = await trace.engine.query(
    `SELECT COUNT(*) AS n FROM thread WHERE upid = ${upid}`,
  );
  const threads = threadsRes.firstRow({n: NUM}).n;

  // Call-stacks and the exact native-snapshot range over the window
  // (baseTs, selTs].
  const {stacks, profileTotal, rangeStart, rangeEnd} = await loadNativeStacks(
    trace,
    upid,
    baseTs,
    snapTs,
  );

  return {
    hasProfile: true,
    stacks,
    profileTotal,
    seen,
    baseSeen,
    nativeRss,
    stackBytes,
    threads: threads > 0 ? threads : undefined,
    rangeStart,
    rangeEnd,
  };
}

// Cumulative positive+negative unreleased bytes for the process up to `toTs`
// (the whole trace when null).
async function cumulativeUnreleased(
  trace: Trace,
  upid: number,
  toTs: bigint | null,
): Promise<number> {
  const res = await trace.engine.query(`
    SELECT CAST(ifnull(SUM(a.size), 0) AS INT) AS seen
    FROM heap_profile_allocation a
    WHERE a.upid = ${upid}${toTs !== null ? ` AND a.ts <= ${toTs}` : ''}
  `);
  return Math.max(0, res.firstRow({seen: NUM}).seen);
}

// Loads the top native allocation call-stacks for one process over the window
// (fromTs, toTs]. Prefer app frames, but retain the nearest meaningful
// framework/library frame as a fallback when a stack has no app frames.
async function loadNativeStacks(
  trace: Trace,
  upid: number,
  fromTs: time | undefined,
  toTs: bigint | null,
): Promise<{
  stacks: NativeStack[];
  profileTotal: number;
  rangeStart?: time;
  rangeEnd?: time;
}> {
  const window =
    (fromTs !== undefined ? ` AND a.ts > ${fromTs}` : '') +
    (toTs !== null ? ` AND a.ts <= ${toTs}` : '');
  const unrelCte = `
    unrel AS (
      SELECT
        a.callsite_id AS callsite_id,
        SUM(a.size) AS unreleased,
        SUM(CASE WHEN a.count > 0 THEN a.count ELSE 0 END) AS allocs
      FROM heap_profile_allocation a
      WHERE a.upid = ${upid}${window}
      GROUP BY a.callsite_id
      HAVING SUM(a.size) > 0
    )`;

  const stacks: NativeStack[] = [];
  const stackRes = await trace.engine.query(`
    WITH
    ${unrelCte},
    top_sites AS (
      SELECT * FROM (
        SELECT u.*, ROW_NUMBER() OVER (
          ORDER BY u.unreleased DESC
        ) AS rn
        FROM unrel u
      ) WHERE rn <= 5
    ),
    chain(top_id, unreleased, allocs, callsite_id, depth) AS (
      SELECT callsite_id, unreleased, allocs, callsite_id, 0
      FROM top_sites
      UNION ALL
      SELECT c.top_id, c.unreleased, c.allocs, sc.parent_id, c.depth + 1
      FROM chain c
      JOIN stack_profile_callsite sc ON sc.id = c.callsite_id
      WHERE sc.parent_id IS NOT NULL AND c.depth < 128
    )
    SELECT
      c.top_id AS top_id,
      CAST(c.unreleased AS INT) AS unreleased,
      CAST(c.allocs AS INT) AS allocs,
      c.depth AS depth,
      coalesce(f.deobfuscated_name, f.name, '<unknown>') AS frame_name,
      mp.name AS mapping_name
    FROM chain c
    JOIN stack_profile_callsite sc ON sc.id = c.callsite_id
    JOIN stack_profile_frame f ON sc.frame_id = f.id
    LEFT JOIN stack_profile_mapping mp ON f.mapping = mp.id
    ORDER BY c.top_id, c.depth ASC
  `);
  let cur: (NativeStack & {topId: number}) | undefined;
  for (
    const it = stackRes.iter({
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
    if (cur === undefined || cur.topId !== it.top_id) {
      cur = {
        topId: it.top_id,
        unreleased: it.unreleased,
        allocs: it.allocs,
        frames: [],
      };
      stacks.push(cur);
    }
    if (isAllocatorPlumbing(it.frame_name)) continue;
    const mapping = it.mapping_name ?? undefined;
    if (isAppMapping(mapping)) {
      cur.frames.push(frameLabel(it.frame_name, mapping));
    } else if (cur.fallbackFrame === undefined) {
      cur.fallbackFrame = frameLabel(it.frame_name, mapping);
    }
  }
  stacks.sort((a, b) => b.unreleased - a.unreleased);

  const totRes = await trace.engine.query(`
    WITH ${unrelCte}
    SELECT
      CAST(ifnull(SUM(unreleased), 0) AS INT) AS total,
      (SELECT MIN(a.ts) FROM heap_profile_allocation a
        WHERE a.upid = ${upid}${window}) AS range_start,
      (SELECT MAX(a.ts) FROM heap_profile_allocation a
        WHERE a.upid = ${upid}${window}) AS range_end
    FROM unrel
  `);
  const totals = totRes.firstRow({
    total: NUM,
    range_start: LONG_NULL,
    range_end: LONG_NULL,
  });

  return {
    stacks,
    profileTotal: totals.total,
    rangeStart:
      totals.range_start !== null
        ? Time.fromRaw(totals.range_start)
        : undefined,
    rangeEnd:
      totals.range_end !== null ? Time.fromRaw(totals.range_end) : undefined,
  };
}

// True when a frame's mapping belongs to the app under test rather than a
// system/runtime library. App code is installed under /data/app or /data/data.
function isAppMapping(mapping?: string): boolean {
  if (mapping === undefined) return false;
  return /^\/data\/(app|data)\//.test(mapping);
}

// Removes low-level allocation entry points which identify how memory was
// allocated, but not which component requested it.
function isAllocatorPlumbing(name: string): boolean {
  const n = name.toLowerCase();
  return (
    /(^|::|__)(malloc|calloc|realloc|memalign|aligned_alloc|posix_memalign)(@.*|\(.*\))?$/.test(
      n,
    ) ||
    /(^|::)operator new(\[\])?(\(.*\))?$/.test(n) ||
    n.includes('malloc_hook') ||
    n.includes('heapprofd') ||
    n.includes('scudo::') ||
    n.includes('jemalloc') ||
    /^je_(malloc|calloc|realloc|memalign)/.test(n)
  );
}

// Prefix non-app frames with their binary name so a framework/library fallback
// remains useful even when its function name is ambiguous or unavailable.
function frameLabel(name: string, mapping?: string): string {
  if (mapping === undefined || isAppMapping(mapping)) return name;
  const binary = mapping.slice(mapping.lastIndexOf('/') + 1);
  return name === '<unknown>' ? binary : `${binary} · ${name}`;
}

// Shrinks an app frame chain (leaf → root) to a short snippet, or shows the
// nearest meaningful framework/library caller when no app frame was captured.
function stackSnippet(frames: string[], fallbackFrame?: string): string[] {
  if (frames.length === 0) {
    return [fallbackFrame ?? '(allocation origin unresolved)'];
  }
  if (frames.length <= 7) return frames;
  const leaf = frames[0];
  const middle = frames.slice(1, -2).slice(0, 4);
  const root = frames.slice(-2);
  return [leaf, ...middle, '…', ...root];
}

export interface NativeSectionAttrs {
  readonly trace: Trace;
  readonly upid: number;
  // The page-wide selection (by ts). Undefined → whole trace / latest.
  readonly selTs?: time;
  readonly baseTs?: time;
}

export class NativeSection implements m.ClassComponent<NativeSectionAttrs> {
  private readonly slot = new AsyncMemo<NativeData>();

  onremove() {
    this.slot.dispose();
  }

  view({attrs}: m.Vnode<NativeSectionAttrs>): m.Children {
    const {trace, upid, selTs, baseTs} = attrs;
    const data = this.slot.use({
      key: {
        traceId: trace.traceInfo.uuid,
        upid,
        sel: selTs !== undefined ? selTs.toString() : null,
        base: baseTs !== undefined ? baseTs.toString() : null,
      },
      compute: () => loadNativeData(trace, upid, selTs, baseTs),
      // Keep the previous window's data while the new one loads, so the panel
      // doesn't flash empty on every snapshot change.
      retainOn: ['sel', 'base'],
    }).data;
    if (data === undefined) {
      return loadingPanel({title: TITLE, subtitle: SUBTITLE}); // Still loading.
    }

    if (!data.hasProfile) {
      return emptyPanel({
        title: TITLE,
        subtitle: SUBTITLE,
        message:
          'No native heap profile (heapprofd) data in this trace for this ' +
          'process.',
      });
    }

    const {seen, baseSeen, nativeRss, stackBytes, threads, profileTotal} = data;
    const comparing = baseTs !== undefined && baseSeen !== undefined;
    const coveragePct = nativeRss > 0 ? (seen / nativeRss) * 100 : undefined;
    const overhead = nativeRss > seen ? nativeRss - seen : 0;

    const stackRows = data.stacks.slice(0, 5).map((s) => [
      m(
        '.pf-memscope-stack',
        stackSnippet(s.frames, s.fallbackFrame).map((f) =>
          m('.pf-memscope-stack__frame', {title: f}, f),
        ),
      ),
      formatBytes(s.unreleased),
      s.allocs.toLocaleString(),
      m(ShareBar, {frac: profileTotal > 0 ? s.unreleased / profileTotal : 0}),
    ]);

    return m(
      Panel,
      m(Panel.Header, {
        title: TITLE,
        subtitle: SUBTITLE,
        controls: showAreaInTimelineLink(
          trace,
          findProcessTrack(trace, upid, (k) => k.startsWith('heap_profile:'))
            ?.uri,
          data.rangeStart,
          data.rangeEnd,
        ),
      }),
      m(
        Panel.Body,
        m(Stack, {spacing: 'large'}, [
          m(
            Callout,
            {intent: Intent.Warning},
            'The native profiler only sees allocations made ',
            m('b', 'after'),
            " tracing started — it can't explain the past. Leaks present " +
              'before t=0 are invisible here; trust the composition totals ' +
              'above for the full resident picture.',
          ),
          m(BillboardStrip, [
            nativeRss > 0 &&
              statCard([
                {
                  label: 'RSS anon + swap',
                  value: formatBytes(nativeRss),
                  sub: 'total native private footprint',
                },
              ]),
            statCard([
              {
                label: 'Seen by profiler',
                value: formatBytes(seen),
                sub:
                  comparing && baseSeen !== undefined
                    ? deltaText(
                        seen - baseSeen,
                        `Δ ${formatDelta(seen - baseSeen)} vs baseline`,
                      )
                    : coveragePct !== undefined
                      ? `${Math.round(coveragePct)}% coverage`
                      : 'unreleased over the whole trace',
              },
            ]),
            overhead > 0 &&
              statCard([
                {
                  label: 'Allocator overhead + unseen',
                  value: formatBytes(overhead),
                  sub: 'metadata, fragmentation, pre-trace',
                },
              ]),
            stackBytes > 0 &&
              statCard([
                {
                  label: 'Thread stacks',
                  value: formatBytes(stackBytes),
                  sub: threads !== undefined ? `${threads} threads` : undefined,
                },
              ]),
          ]),
          coveragePct !== undefined &&
            m(Ratio, {
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
          stackRows.length > 0
            ? topTable({
                title: 'Top allocation call-stacks',
                subtitle: comparing
                  ? 'unreleased between baseline and selected snapshot · ' +
                    'app frames preferred'
                  : selTs !== undefined
                    ? 'unreleased up to the selected snapshot · ' +
                      'app frames preferred'
                    : 'unreleased over the whole trace · app frames preferred',
                cols: [
                  {label: 'Call-stack snippet'},
                  {label: comparing ? 'Δ unreleased' : 'Unreleased', num: true},
                  {label: 'Allocs', num: true},
                  {label: 'Share of profiled', num: true},
                ],
                rows: stackRows,
              })
            : m(
                '.pf-memscope-placeholder',
                comparing
                  ? 'No unreleased allocations between these snapshots.'
                  : 'No unreleased allocations captured by the profiler here.',
              ),
        ]),
      ),
    );
  }
}
