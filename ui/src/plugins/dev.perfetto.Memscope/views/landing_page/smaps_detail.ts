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

// "Smaps detail" tab: every mapping of the selected snapshot (a dropdown picks
// which; defaults to the latest), folded into the two-level taxonomy (or flat),
// with regex filtering, optional extra columns and collapsible groups.
// Self-contained: owns its query slots and loading.

import m from 'mithril';
import {QuerySlot} from '../../../../base/query_slot';
import type {Trace} from '../../../../public/trace';
import {LONG, NUM, STR} from '../../../../trace_processor/query_result';
import {Button} from '../../../../widgets/button';
import {Icon} from '../../../../widgets/icon';
import {RadioGroup} from '../../../../widgets/radio_group';
import {Select} from '../../../../widgets/select';
import {TextInput} from '../../../../widgets/text_input';
import {Panel} from '../../components/panel';
import {formatBytes, statCard} from './mem_format';
import {emptyPanel, loadingPanel} from './section_widgets';
import {MEMMAP_GREY} from './smaps_categories';
import {BillboardStrip} from '../../components/billboard';
import {Table} from '../../components/table';
import {Stack} from '../../../../widgets/stack';

const TITLE = 'Every mapping, grouped';
const SUBTITLE =
  'The raw /proc/<pid>/smaps dump, folded into the same taxonomy as the ' +
  'composition above. Filter by regex, or flatten to see individual mappings.';

// One smaps path (VMA group) at the latest snapshot, with the full residency
// split.
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

// Two-level taxonomy for the tree view: top-level groups and their
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

// Classifies one smaps path into a (group, sub) of SMAPS_TREE. Order matters:
// graphics devices before the generic file-backed '/' check, and the dalvik
// heap spaces before the generic dalvik bucket.
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

// One smaps snapshot for the picker: its ts and total resident bytes.
interface SmapsSnapshotInfo {
  ts: bigint;
  rss: number;
}

// All smaps snapshots for one process (ts-ascending) with their total RSS, to
// populate the snapshot dropdown.
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
    out.push({ts: it.ts, rss: it.rss});
  }
  return out;
}

// Per-path residency at the given smaps snapshot of one process. When `ts` is
// undefined, falls back to the latest snapshot.
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
      path: STR,
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
      path: it.path,
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
}

export class SmapsDetail implements m.ClassComponent<SmapsDetailAttrs> {
  private readonly slot = new QuerySlot<SmapsPathRow[]>();
  private readonly snapshotsSlot = new QuerySlot<SmapsSnapshotInfo[]>();
  // View state.
  private smapsFlat = false;
  private smapsAllCols = false;
  private smapsFilter = '';
  // The selected snapshot ts, or undefined to follow the latest snapshot.
  private selectedTs?: bigint;
  // Collapsed group/subgroup keys in the tree.
  private readonly collapsed = new Set<string>();

  onremove() {
    this.slot.dispose();
    this.snapshotsSlot.dispose();
  }

  view({attrs}: m.Vnode<SmapsDetailAttrs>): m.Children {
    const {trace, upid} = attrs;
    const snapshots =
      this.snapshotsSlot.use({
        key: {traceId: trace.traceInfo.uuid, upid},
        queryFn: () => loadSmapsSnapshots(trace, upid),
      }).data ?? [];
    const rows = this.slot.use({
      key: {
        traceId: trace.traceInfo.uuid,
        upid,
        ts: this.selectedTs?.toString() ?? 'latest',
      },
      queryFn: () => loadSmapsPaths(trace, upid, this.selectedTs),
    }).data;
    if (rows === undefined) {
      return m(
        '.pf-memscope-charts',
        loadingPanel({title: TITLE, subtitle: SUBTITLE}),
      ); // Still loading.
    }
    if (rows.length === 0) {
      return emptyPanel({
        title: TITLE,
        subtitle: SUBTITLE,
        message: 'No smaps data in this trace for this process.',
      });
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
                  m('span.pf-memscope-smaps__count', `· ${s.rows.length} maps`),
                ]),
                sumCells(s.rows),
              ],
            ),
          );
          if (this.collapsed.has(subKey)) continue;
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
      m(Panel.Header, {title: TITLE, subtitle: SUBTITLE}),
      m(
        Panel.Body,
        m(Stack, {spacing: 'large'}, [
          m(BillboardStrip, [
            statCard([
              {
                label: 'Total RSS',
                value: formatBytes(sum(rows, (r) => r.rss)),
                sub: `${rows.length} mappings`,
              },
            ]),
            statCard([
              {
                label: 'RSS anon + swap',
                value: formatBytes(sum(rows, (r) => r.anon + r.swap)),
                sub: 'private anon + swapped',
              },
            ]),
            statCard([
              {
                label: 'Total PSS',
                value: formatBytes(sum(rows, (r) => r.pss)),
                sub: 'proportional set size',
              },
            ]),
            statCard([
              {
                label: 'Private dirty',
                value: formatBytes(sum(rows, (r) => r.privateDirty)),
                sub: 'unshareable cost',
              },
            ]),
            statCard([
              {
                label: 'Swap',
                value: formatBytes(sum(rows, (r) => r.swap)),
                sub: 'zram / swapped out',
              },
            ]),
          ]),
          m('.pf-memscope-smaps__controls', [
            snapshots.length > 1 &&
              m(
                Select,
                {
                  // Default to the latest snapshot when nothing is picked.
                  value: (
                    this.selectedTs ?? snapshots[snapshots.length - 1].ts
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
                    `#${i + 1} · t=${secs.toFixed(0)}s · ${formatBytes(s.rss)}`,
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
              `${filtered.length} / ${rows.length} mappings`,
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
