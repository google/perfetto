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
import {
  MergedSlice,
  SortState,
  SummaryRow,
  LONG_PKG_PREFIX,
} from '../models/types';
import {stateColor, stateLabel, nameColor} from '../utils/colors';
import {fmtDur, fmtPct} from '../utils/format';

function buildSummaryData(data: MergedSlice[]): {
  stateMap: Record<string, {dur: number; count: number; color: string}>;
  nameMap: Record<string, {dur: number; count: number}>;
  bfMap: Record<string, {dur: number; count: number}>;
} {
  const stateMap: Record<string, {dur: number; count: number; color: string}> =
    {};
  const nameMap: Record<string, {dur: number; count: number}> = {};
  const bfMap: Record<string, {dur: number; count: number}> = {};

  for (const d of data) {
    const sk = stateLabel(d);
    if (stateMap[sk] === undefined) {
      stateMap[sk] = {dur: 0, count: 0, color: stateColor(d)};
    }
    stateMap[sk].dur += d.dur;
    stateMap[sk].count += 1;

    if (d.name !== null) {
      if (nameMap[d.name] === undefined) {
        nameMap[d.name] = {dur: 0, count: 0};
      }
      nameMap[d.name].dur += d.dur;
      nameMap[d.name].count += 1;
    }

    if (d.blocked_function !== null) {
      if (bfMap[d.blocked_function] === undefined) {
        bfMap[d.blocked_function] = {dur: 0, count: 0};
      }
      bfMap[d.blocked_function].dur += d.dur;
      bfMap[d.blocked_function].count += 1;
    }
  }
  return {stateMap, nameMap, bfMap};
}

type SortCol = 'label' | 'dur' | 'pct' | 'count';

function sortRows(rows: SummaryRow[], sort: SortState): SummaryRow[] {
  return [...rows].sort((a, b) => {
    const col = sort.col as SortCol;
    let va: string | number = a[col];
    let vb: string | number = b[col];
    if (col === 'label') {
      va = (va as string).toLowerCase();
      vb = (vb as string).toLowerCase();
    }
    if (va < vb) return -1 * sort.dir;
    if (va > vb) return 1 * sort.dir;
    return 0;
  });
}

interface TableCardAttrs {
  id: string;
  title: string;
  rows: SummaryRow[];
  maxDur: number;
  totalDur: number;
  sortState: Record<string, SortState>;
}

const TableCard: m.Component<TableCardAttrs> = {
  view(vnode: m.Vnode<TableCardAttrs>) {
    const {id, title, rows, maxDur, totalDur, sortState} = vnode.attrs;
    if (sortState[id] === undefined) sortState[id] = {col: 'dur', dir: -1};
    const sort = sortState[id];
    const sorted = sortRows(rows, sort);

    const cols: Array<{col: string; label: string}> = [
      {col: 'label', label: 'Name'},
      {col: 'dur', label: 'Duration'},
      {col: 'pct', label: '%'},
      {col: 'count', label: 'Count'},
    ];

    function onHeaderClick(col: string): void {
      if (sort.col === col) {
        sort.dir = (sort.dir === -1 ? 1 : -1) as 1 | -1;
      } else {
        sort.col = col;
        sort.dir = col === 'label' ? 1 : -1;
      }
    }

    return m('.qs-card.qs-table-card', [
      m('.qs-table-card-head', m('span', title)),
      m(
        '.qs-table-scroll',
        m('table.qs-summary', [
          m(
            'thead',
            m(
              'tr',
              cols.map((c) =>
                m(
                  'th',
                  {
                    class: sort.col === c.col ? 'sorted' : '',
                    onclick: () => onHeaderClick(c.col),
                  },
                  [
                    c.label,
                    ' ',
                    m(
                      'span.qs-sort-arrow',
                      sort.col === c.col
                        ? sort.dir === -1
                          ? '\u2193'
                          : '\u2191'
                        : '\u2195',
                    ),
                  ],
                ),
              ),
            ),
          ),
          m(
            'tbody',
            sorted.map((row) =>
              m('tr', [
                m(
                  'td',
                  m('.qs-cell-label', [
                    row.color
                      ? m('span.qs-swatch', {
                          style: {background: row.color},
                        })
                      : null,
                    m('div', [
                      m(
                        'span.qs-name-text',
                        {title: row.label},
                        row.short || row.label,
                      ),
                      m(
                        '.qs-bar-wrap',
                        m('.qs-bar-fill', {
                          style: {
                            width: ((row.dur / maxDur) * 100).toFixed(1) + '%',
                            background: row.color || 'var(--accent)',
                          },
                        }),
                      ),
                    ]),
                  ]),
                ),
                m('td', fmtDur(row.dur)),
                m('td', fmtPct(row.dur, totalDur)),
                m('td', String(row.count)),
              ]),
            ),
          ),
        ]),
      ),
    ]);
  },
};

/**
 * Minimal interface for the trace-level state that SummaryTables needs.
 */
export interface SummaryTrace {
  currentSeq: MergedSlice[];
  totalDur: number;
}

interface SummaryAttrs {
  ts: SummaryTrace;
  /** Mutable sort state record, shared with the parent to persist across renders. */
  sortState: Record<string, SortState>;
}

export const SummaryTables: m.Component<SummaryAttrs> = {
  view(vnode: m.Vnode<SummaryAttrs>) {
    const {ts, sortState} = vnode.attrs;
    const data = ts.currentSeq;
    const totalDur = ts.totalDur;
    const {stateMap, nameMap, bfMap} = buildSummaryData(data);

    const tables: m.Children[] = [];

    const maxState = Math.max(...Object.values(stateMap).map((v) => v.dur), 1);
    const stateRows: SummaryRow[] = Object.entries(stateMap).map(([k, v]) => ({
      label: k,
      short: k,
      dur: v.dur,
      count: v.count,
      color: v.color,
      pct: totalDur > 0 ? (v.dur / totalDur) * 100 : 0,
    }));
    tables.push(
      m(TableCard, {
        id: 'state',
        title: 'States',
        rows: stateRows,
        maxDur: maxState,
        totalDur,
        sortState,
      }),
    );

    if (Object.keys(nameMap).length) {
      const maxName = Math.max(...Object.values(nameMap).map((v) => v.dur), 1);
      const nameRows: SummaryRow[] = Object.entries(nameMap).map(([k, v]) => ({
        label: k,
        short: k.replace(LONG_PKG_PREFIX, ''),
        dur: v.dur,
        count: v.count,
        color: nameColor(k),
        pct: totalDur > 0 ? (v.dur / totalDur) * 100 : 0,
      }));
      tables.push(
        m(TableCard, {
          id: 'name',
          title: 'Names',
          rows: nameRows,
          maxDur: maxName,
          totalDur,
          sortState,
        }),
      );
    }

    if (Object.keys(bfMap).length) {
      const maxBf = Math.max(...Object.values(bfMap).map((v) => v.dur), 1);
      const bfRows: SummaryRow[] = Object.entries(bfMap).map(([k, v]) => ({
        label: k,
        short: k,
        dur: v.dur,
        count: v.count,
        color: '#c62828',
        pct: totalDur > 0 ? (v.dur / totalDur) * 100 : 0,
      }));
      tables.push(
        m(TableCard, {
          id: 'bf',
          title: 'Blocked functions',
          rows: bfRows,
          maxDur: maxBf,
          totalDur,
          sortState,
        }),
      );
    }

    return m('.qs-summary-grid', tables);
  },
};
