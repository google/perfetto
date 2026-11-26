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

// "Memory Map (Smaps)" section: every VMA mapping of the selected snapshot
// (driven by the top Memory Timeline when `selTs` is provided, or its own
// dropdown otherwise), folded into the two-level taxonomy (or flat), with
// regex filtering, optional extra columns, collapsible groups, and full Δ
// comparison mode when a time range (`baseTs` → `selTs`) is brushed.

import m from 'mithril';
import {AsyncMemo} from '../../../../base/async_memo';
import {Time, type time} from '../../../../base/time';
import type {Trace} from '../../../../public/trace';
import {LONG, NUM, STR_NULL} from '../../../../trace_processor/query_result';
import {Button} from '../../../../widgets/button';
import {Icon} from '../../../../widgets/icon';
import {RadioGroup} from '../../../../widgets/radio_group';
import {Select} from '../../../../widgets/select';
import {Stack} from '../../../../widgets/stack';
import {TextInput} from '../../../../widgets/text_input';
import {BillboardStrip} from '../../components/billboard';
import {Panel} from '../../components/panel';
import {Table} from '../../components/table';
import {deltaText, formatBytes, formatDelta, statCard} from './mem_format';
import {deltaCell, emptyPanel, loadingPanel} from './section_widgets';
import {nearestByTs} from './selection';
import {MEMMAP_GREY} from './smaps_categories';

const TITLE = 'Virtual Memory Mappings (Smaps)';

interface SmapsPathRow {
  path: string;
  rss: number;
  pss: number;
  anon: number;
  swap: number;
  privateDirty: number;
  privateClean: number;
  sharedDirty: number;
  sharedClean: number;
}

const ZERO_ROW: SmapsPathRow = {
  path: '',
  rss: 0,
  pss: 0,
  anon: 0,
  swap: 0,
  privateDirty: 0,
  privateClean: 0,
  sharedDirty: 0,
  sharedClean: 0,
};

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

interface SmapsSnapshotInfo {
  readonly ts: time;
  readonly rss: number;
}

async function loadSmapsSnapshots(
  trace: Trace,
  upid: number,
): Promise<SmapsSnapshotInfo[]> {
  const out: SmapsSnapshotInfo[] = [];
  const res = await trace.engine.query(`
    SELECT s.ts AS ts, CAST(ifnull(SUM(s.rss_kb), 0) * 1024 AS INT) AS rss
    FROM profiler_smaps s
    WHERE s.upid = ${upid}
    GROUP BY s.ts
    ORDER BY s.ts ASC
  `);
  for (const it = res.iter({ts: LONG, rss: NUM}); it.valid(); it.next()) {
    out.push({ts: Time.fromRaw(it.ts), rss: it.rss});
  }
  return out;
}

async function loadSmapsPaths(
  trace: Trace,
  upid: number,
  ts: bigint | undefined,
): Promise<SmapsPathRow[]> {
  const tsFilter =
    ts !== undefined
      ? `s.ts = ${ts}`
      : `s.ts = (SELECT MAX(ts) FROM profiler_smaps WHERE upid = ${upid})`;
  const rows: SmapsPathRow[] = [];
  const res = await trace.engine.query(`
    SELECT
      s.path AS path,
      CAST(ifnull(SUM(s.rss_kb), 0) * 1024 AS INT) AS rss,
      CAST(ifnull(SUM(s.proportional_resident_kb), 0) * 1024 AS INT) AS pss,
      CAST(ifnull(SUM(s.anonymous_kb), 0) * 1024 AS INT) AS anon,
      CAST(ifnull(SUM(s.swap_kb), 0) * 1024 AS INT) AS swap,
      CAST(ifnull(SUM(s.private_dirty_kb), 0) * 1024 AS INT) AS private_dirty,
      CAST(ifnull(SUM(s.private_clean_resident_kb), 0) * 1024 AS INT)
        AS private_clean,
      CAST(ifnull(SUM(s.shared_dirty_resident_kb), 0) * 1024 AS INT)
        AS shared_dirty,
      CAST(ifnull(SUM(s.shared_clean_resident_kb), 0) * 1024 AS INT)
        AS shared_clean
    FROM profiler_smaps s
    WHERE s.upid = ${upid} AND ${tsFilter}
    GROUP BY s.path
    ORDER BY rss DESC
  `);
  for (
    const it = res.iter({
      path: STR_NULL,
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
    rows.push({
      path: it.path ?? '',
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
  return rows;
}

export interface SmapsDetailAttrs {
  readonly trace: Trace;
  readonly upid: number;
  // Optional page-wide timeline selection. When provided, SmapsDetail aligns
  // to `selTs` and shows Δ vs `baseTs` when a range is dragged.
  readonly selTs?: time;
  readonly baseTs?: time;
  readonly hideSnapshotPicker?: boolean;
}

interface MergedPathRow {
  readonly path: string;
  readonly cur: SmapsPathRow;
  readonly base?: SmapsPathRow;
}

export class SmapsDetail implements m.ClassComponent<SmapsDetailAttrs> {
  private readonly curSlot = new AsyncMemo<SmapsPathRow[]>();
  private readonly baseSlot = new AsyncMemo<SmapsPathRow[]>();
  private readonly snapshotsSlot = new AsyncMemo<SmapsSnapshotInfo[]>();
  private smapsFlat = false;
  private smapsAllCols = false;
  private smapsFilter = '';
  private selectedTs?: bigint;
  private readonly collapsed = new Set<string>();

  onremove() {
    this.curSlot.dispose();
    this.baseSlot.dispose();
    this.snapshotsSlot.dispose();
  }

  view({attrs}: m.Vnode<SmapsDetailAttrs>): m.Children {
    const {trace, upid, selTs, baseTs, hideSnapshotPicker = false} = attrs;
    const snapshots =
      this.snapshotsSlot.use({
        key: {traceId: trace.traceInfo.uuid, upid},
        compute: () => loadSmapsSnapshots(trace, upid),
      }).data ?? [];

    // Resolve active snapshot ts: external timeline selection takes precedence.
    const resolvedCurSnap =
      snapshots.length > 0
        ? selTs !== undefined
          ? nearestByTs(snapshots, selTs)
          : this.selectedTs !== undefined
            ? (snapshots.find((s) => s.ts === this.selectedTs) ??
              snapshots[snapshots.length - 1])
            : snapshots[snapshots.length - 1]
        : undefined;

    const resolvedBaseSnap =
      baseTs !== undefined && snapshots.length > 0
        ? nearestByTs(snapshots, baseTs)
        : undefined;

    const comparing =
      resolvedCurSnap !== undefined &&
      resolvedBaseSnap !== undefined &&
      resolvedBaseSnap.ts !== resolvedCurSnap.ts;

    const curTs = resolvedCurSnap?.ts ?? this.selectedTs;
    const rows = this.curSlot.use({
      key: {
        traceId: trace.traceInfo.uuid,
        upid,
        ts: curTs?.toString() ?? 'latest',
      },
      compute: () => loadSmapsPaths(trace, upid, curTs),
    }).data;

    const baseRows = comparing
      ? this.baseSlot.use({
          key: {
            traceId: trace.traceInfo.uuid,
            upid,
            ts: resolvedBaseSnap.ts.toString(),
          },
          compute: () => loadSmapsPaths(trace, upid, resolvedBaseSnap.ts),
        }).data
      : undefined;

    if (rows === undefined || (comparing && baseRows === undefined)) {
      return m('.pf-memscope-charts', loadingPanel({title: TITLE}));
    }
    if (
      rows.length === 0 &&
      (baseRows === undefined || baseRows.length === 0)
    ) {
      return emptyPanel({
        title: TITLE,
        message: 'No smaps data in this trace for this process.',
      });
    }

    // Build merged rows (union of current & baseline paths when comparing).
    const baseByPath = new Map<string, SmapsPathRow>();
    if (baseRows !== undefined) {
      for (const r of baseRows) {
        baseByPath.set(r.path, r);
      }
    }
    const merged: MergedPathRow[] = [];
    const seenPaths = new Set<string>();
    for (const r of rows) {
      seenPaths.add(r.path);
      merged.push({
        path: r.path,
        cur: r,
        base: comparing ? baseByPath.get(r.path) : undefined,
      });
    }
    if (comparing && baseRows !== undefined) {
      for (const br of baseRows) {
        if (!seenPaths.has(br.path)) {
          merged.push({
            path: br.path,
            cur: {...ZERO_ROW, path: br.path},
            base: br,
          });
        }
      }
    }

    const sumCur = (
      list: readonly MergedPathRow[],
      get: (r: SmapsPathRow) => number,
    ) => list.reduce((s, mRow) => s + get(mRow.cur), 0);
    const sumBase = (
      list: readonly MergedPathRow[],
      get: (r: SmapsPathRow) => number,
    ) =>
      list.reduce(
        (s, mRow) => s + (mRow.base !== undefined ? get(mRow.base) : 0),
        0,
      );

    const rowSortScore = (r: MergedPathRow): number => {
      if (!comparing) return r.cur.rss;
      const baseRss = r.base?.rss ?? 0;
      return Math.abs(r.cur.rss - baseRss);
    };

    let filtered = merged;
    if (this.smapsFilter !== '') {
      try {
        const re = new RegExp(this.smapsFilter);
        filtered = merged.filter((r) => re.test(r.path));
      } catch {
        filtered = merged.filter((r) => r.path.includes(this.smapsFilter));
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

    const fmtCell = (n: number) => (n > 0 ? formatBytes(n) : '-');
    const numCells = (r: MergedPathRow) =>
      cols.map((c) => {
        const curVal = c.get(r.cur);
        const baseVal = r.base !== undefined ? c.get(r.base) : undefined;
        return m(
          'td.pf-memscope-table__num',
          deltaCell(
            fmtCell(curVal),
            baseVal !== undefined ? curVal - baseVal : undefined,
            comparing,
          ),
        );
      });
    const sumCells = (list: readonly MergedPathRow[]) =>
      cols.map((c) => {
        const curVal = sumCur(list, c.get);
        const baseVal = comparing ? sumBase(list, c.get) : undefined;
        return m(
          'td.pf-memscope-table__num.pf-memscope-table__size',
          deltaCell(
            fmtCell(curVal),
            baseVal !== undefined ? curVal - baseVal : undefined,
            comparing,
          ),
        );
      });

    const toggle = (key: string) => {
      if (this.collapsed.has(key)) {
        this.collapsed.delete(key);
      } else {
        this.collapsed.add(key);
      }
    };
    const chevron = (key: string) =>
      m(Icon, {
        className: 'pf-memscope-smaps__chevron',
        icon: this.collapsed.has(key) ? 'chevron_right' : 'expand_more',
      });
    const swatch = (color: string) =>
      m('span.pf-memscope-growth__swatch', {style: {background: color}});

    const MAX_CHILD_ROWS = 30;
    const pathRow = (r: MergedPathRow, indent: string) =>
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
      const sortedFlat = filtered
        .slice()
        .sort((a, b) => rowSortScore(b) - rowSortScore(a));
      body = sortedFlat
        .slice(0, 200)
        .map((r) => pathRow(r, 'pf-memscope-smaps__lvl0'));
      if (sortedFlat.length > 200) {
        body.push(moreRow(sortedFlat.length - 200, 'pf-memscope-smaps__lvl0'));
      }
    } else {
      const buckets = new Map<string, MergedPathRow[]>();
      for (const r of filtered) {
        const {group, sub} = classifyMapping(r.path);
        const key = `${group}/${sub}`;
        const list = buckets.get(key) ?? [];
        list.push(r);
        buckets.set(key, list);
      }

      const groupScore = (list: readonly MergedPathRow[]): number =>
        comparing
          ? Math.abs(sumCur(list, (r) => r.rss) - sumBase(list, (r) => r.rss))
          : sumCur(list, (r) => r.rss);

      body = [];
      for (const g of SMAPS_TREE) {
        const subLists = g.subs
          .map((s) => ({
            meta: s,
            rows: buckets.get(`${g.key}/${s.key}`) ?? [],
          }))
          .filter((s) => s.rows.length > 0)
          .sort((a, b) => groupScore(b.rows) - groupScore(a.rows));
        if (subLists.length === 0) continue;
        const groupRows = subLists.flatMap((s) => s.rows);

        body.push(
          m('tr.pf-memscope-smaps__group', {onclick: () => toggle(g.key)}, [
            m('td', [chevron(g.key), swatch(g.color), ` ${g.label}`]),
            sumCells(groupRows),
          ]),
        );
        if (this.collapsed.has(g.key)) continue;

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
                  m('span.pf-memscope-smaps__count', `- ${s.rows.length} maps`),
                ]),
                sumCells(s.rows),
              ],
            ),
          );
          if (this.collapsed.has(subKey)) continue;
          const sorted = s.rows
            .slice()
            .sort((a, b) => rowSortScore(b) - rowSortScore(a));
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

    const tOf = (ts: time) =>
      (Number(ts - trace.traceInfo.start) / 1e9).toFixed(1);
    const curIdx =
      resolvedCurSnap !== undefined ? snapshots.indexOf(resolvedCurSnap) : -1;
    const baseIdx =
      resolvedBaseSnap !== undefined ? snapshots.indexOf(resolvedBaseSnap) : -1;

    const totalRss = sumCur(merged, (r) => r.rss);
    const baseRss = comparing ? sumBase(merged, (r) => r.rss) : 0;
    const totalAnonSwap = sumCur(merged, (r) => r.anon + r.swap);
    const baseAnonSwap = comparing
      ? sumBase(merged, (r) => r.anon + r.swap)
      : 0;
    const totalPss = sumCur(merged, (r) => r.pss);
    const basePss = comparing ? sumBase(merged, (r) => r.pss) : 0;
    const totalPrivDirty = sumCur(merged, (r) => r.privateDirty);
    const basePrivDirty = comparing
      ? sumBase(merged, (r) => r.privateDirty)
      : 0;
    const totalSwap = sumCur(merged, (r) => r.swap);
    const baseSwap = comparing ? sumBase(merged, (r) => r.swap) : 0;

    return m(
      Panel,
      m(Panel.Header, {title: TITLE}),
      m(
        Panel.Body,
        m(Stack, {spacing: 'large'}, [
          resolvedCurSnap !== undefined &&
            m('.pf-memscope-memmap__section-header', [
              m(
                'span.pf-memscope-memmap__section-title',
                {
                  className: comparing
                    ? 'pf-memscope-memmap__section-title--diff'
                    : 'pf-memscope-memmap__section-title--straight',
                },
                comparing && resolvedBaseSnap !== undefined
                  ? `Diffing snapshot #${baseIdx + 1} (t=${tOf(resolvedBaseSnap.ts)}s) → #${curIdx + 1} (t=${tOf(resolvedCurSnap.ts)}s)`
                  : `Snapshot #${curIdx + 1} · t=${tOf(resolvedCurSnap.ts)}s`,
              ),
              m(
                'span.pf-memscope-memmap__section-hint',
                comparing
                  ? 'sorted by |Δ RSS| — biggest movers first'
                  : `${rows.length} mappings · sorted by RSS`,
              ),
            ]),
          m(BillboardStrip, [
            statCard([
              {
                label: 'Total RSS',
                value: formatBytes(totalRss),
                sub: comparing
                  ? deltaText(
                      totalRss - baseRss,
                      `Δ ${formatDelta(totalRss - baseRss)} vs baseline`,
                    )
                  : `${rows.length} mappings`,
              },
            ]),
            statCard([
              {
                label: 'RSS anon + swap',
                value: formatBytes(totalAnonSwap),
                sub: comparing
                  ? deltaText(
                      totalAnonSwap - baseAnonSwap,
                      `Δ ${formatDelta(totalAnonSwap - baseAnonSwap)} vs baseline`,
                    )
                  : 'private anon + swapped',
              },
            ]),
            statCard([
              {
                label: 'Total PSS',
                value: formatBytes(totalPss),
                sub: comparing
                  ? deltaText(
                      totalPss - basePss,
                      `Δ ${formatDelta(totalPss - basePss)} vs baseline`,
                    )
                  : 'proportional set size',
              },
            ]),
            statCard([
              {
                label: 'Private dirty',
                value: formatBytes(totalPrivDirty),
                sub: comparing
                  ? deltaText(
                      totalPrivDirty - basePrivDirty,
                      `Δ ${formatDelta(totalPrivDirty - basePrivDirty)} vs baseline`,
                    )
                  : 'unshareable cost',
              },
            ]),
            statCard([
              {
                label: 'Swap',
                value: formatBytes(totalSwap),
                sub: comparing
                  ? deltaText(
                      totalSwap - baseSwap,
                      `Δ ${formatDelta(totalSwap - baseSwap)} vs baseline`,
                    )
                  : 'zram / swapped out',
              },
            ]),
          ]),
          m('.pf-memscope-smaps__controls', [
            !hideSnapshotPicker &&
              snapshots.length > 1 &&
              m(
                Select,
                {
                  value: (
                    resolvedCurSnap?.ts ??
                    snapshots[snapshots.length - 1]?.ts ??
                    0n
                  ).toString(),
                  onchange: (e: Event) => {
                    const v = (e.target as HTMLSelectElement).value;
                    this.selectedTs = BigInt(v);
                  },
                },
                snapshots.map((s, i) => {
                  const secs = Number(s.ts - trace.traceInfo.start) / 1e9;
                  return m(
                    'option',
                    {value: s.ts.toString()},
                    `#${i + 1} - t=${secs.toFixed(0)}s - ${formatBytes(s.rss)}`,
                  );
                }),
              ),
            m(
              RadioGroup,
              {
                selectedValue: this.smapsFlat ? 'flat' : 'tree',
                onValueChange: (value: string) =>
                  (this.smapsFlat = value === 'flat'),
              },
              m(
                RadioGroup.Button,
                {value: 'tree', icon: 'account_tree'},
                'Tree',
              ),
              m(
                RadioGroup.Button,
                {value: 'flat', icon: 'format_list_bulleted'},
                'Flat',
              ),
            ),
            m(TextInput, {
              className: 'pf-memscope-smaps__filter',
              leftIcon: 'search',
              placeholder: 'Filter by regex, e.g. \\.so$ or dalvik|scudo',
              value: this.smapsFilter,
              onInput: (value: string) => (this.smapsFilter = value),
            }),
            m(Button, {
              icon: 'view_column',
              label: 'All columns',
              active: this.smapsAllCols,
              onclick: () => (this.smapsAllCols = !this.smapsAllCols),
            }),
            m(
              'span.pf-memscope-smaps__counttext',
              `${filtered.length} / ${merged.length} mappings`,
            ),
          ]),
          m(
            Table,
            {className: 'pf-memscope-table--flush'},
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
      ),
    );
  }
}
