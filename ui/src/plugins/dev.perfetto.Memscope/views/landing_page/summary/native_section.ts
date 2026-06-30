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
import {QuerySlot} from '../../../../../base/query_slot';
import type {time} from '../../../../../base/time';
import type {Trace} from '../../../../../public/trace';
import {
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../../../../trace_processor/query_result';
import {Panel} from '../../../components/panel';
import {Callout} from '../../../components/callout';
import {Intent} from '../../../../../widgets/common';
import {deltaText, formatBytes, formatDelta, statCard} from '../mem_format';
import {findProcessTrack, showInTimelineLink} from '../process_links';
import {
  emptyPanel,
  loadingPanel,
  ratioBar,
  shareBar,
  topTable,
} from '../section_widgets';
import {SMAPS_CATEGORY_CASE_SQL} from '../smaps_categories';
import {Stack} from '../../../../../widgets/stack';
import {BillboardStrip} from '../../../components/billboard';

const TITLE = 'How much native memory did you use, and where did it go?';
const SUBTITLE =
  "Aggregated by allocation callstack — the clearest signal when it's " +
  'available.';

// One top-unreleased heapprofd callsite with its app-only frame chain
// (leaf → root).
interface NativeStack {
  unreleased: number;
  allocs: number;
  frames: string[];
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
  // HeapProfile track event id (MIN object id) at the heapprofd dump nearest
  // the selection — the target for "show in timeline". -1 when no profile.
  readonly eventId: number;
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
      eventId: -1,
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

  // HeapProfile track event id (MIN object id) at the heapprofd dump nearest
  // the selection, so "show in timeline" jumps to the right point on the
  // heapprofd track. heapprofd is continuous (not snapshot-based), so this is
  // the closest dump to the selected window end rather than an exact match.
  const eventRes = await trace.engine.query(`
    SELECT MIN(a.id) AS event_id
    FROM heap_profile_allocation a
    WHERE a.upid = ${upid}
      AND a.ts = (
        SELECT ts FROM heap_profile_allocation
        WHERE upid = ${upid}
        ORDER BY ${snapTs !== null ? `ABS(ts - ${snapTs})` : 'ts'}
        LIMIT 1
      )
  `);
  const eventId = eventRes.firstRow({event_id: NUM_NULL}).event_id ?? -1;

  // Call-stacks over the window (baseTs, selTs].
  const {stacks, profileTotal} = await loadNativeStacks(
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
    eventId,
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
// (fromTs, toTs]. Frame chains are filtered to app frames.
async function loadNativeStacks(
  trace: Trace,
  upid: number,
  fromTs: time | undefined,
  toTs: bigint | null,
): Promise<{stacks: NativeStack[]; profileTotal: number}> {
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
    // Keep only frames from the app under test; drop system/runtime libraries.
    if (isAppMapping(it.mapping_name ?? undefined)) {
      cur.frames.push(it.frame_name);
    }
  }
  stacks.sort((a, b) => b.unreleased - a.unreleased);

  const totRes = await trace.engine.query(`
    WITH ${unrelCte}
    SELECT CAST(ifnull(SUM(unreleased), 0) AS INT) AS total FROM unrel
  `);
  const profileTotal = totRes.firstRow({total: NUM}).total;

  return {stacks, profileTotal};
}

// True when a frame's mapping belongs to the app under test rather than a
// system/runtime library. App code is installed under /data/app or /data/data.
function isAppMapping(mapping?: string): boolean {
  if (mapping === undefined) return false;
  return /^\/data\/(app|data)\//.test(mapping);
}

// Shrinks an (already app-only) frame chain (leaf → root) to a short snippet.
function stackSnippet(frames: string[]): string[] {
  if (frames.length === 0) return ['(no app frames)'];
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
  private readonly slot = new QuerySlot<NativeData>();

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
      queryFn: () => loadNativeData(trace, upid, selTs, baseTs),
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
        stackSnippet(s.frames).map((f) =>
          m(
            '.pf-memscope-stack__frame',
            {title: f},
            f.length > 60 ? `${f.slice(0, 58)}…` : f,
          ),
        ),
      ),
      formatBytes(s.unreleased),
      s.allocs.toLocaleString(),
      shareBar(profileTotal > 0 ? s.unreleased / profileTotal : 0),
    ]);

    return m(
      Panel,
      m(Panel.Header, {
        title: TITLE,
        subtitle: SUBTITLE,
        controls: showInTimelineLink(
          trace,
          findProcessTrack(trace, upid, (k) => k.startsWith('heap_profile:'))
            ?.uri,
          data.eventId,
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
            ratioBar({
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
                    'app frames only'
                  : selTs !== undefined
                    ? 'unreleased up to the selected snapshot · app frames only'
                    : 'unreleased over the whole trace · app frames only',
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
                  ? 'No app allocations captured between these snapshots.'
                  : 'No app allocations captured by the profiler here.',
              ),
        ]),
      ),
    );
  }
}
