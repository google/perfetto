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

// Cell renderers + schema factories for diff views. Plain coloured text,
// no pills or chips. aria-labels carry the sign in words so colour isn't
// the only cue.

import m from 'mithril';
import type {
  CellRenderResult,
  CellRenderer,
  ColumnDef,
  SchemaRegistry,
} from '../../../components/widgets/datagrid/datagrid_schema';
import type {SqlValue} from '../../../trace_processor/query_result';
import {fmtSize} from '../format';
import type {DiffStatus, Num} from './diff_rows';
import {
  KEY_COL,
  STATUS_COL,
  baselineCol,
  currentCol,
  deltaCol,
} from './diff_rows';

const STATUS_LABEL: Record<DiffStatus, string> = {
  NEW: 'NEW',
  REMOVED: 'REMOVED',
  GREW: 'GREW',
  SHRANK: 'SHRANK',
  UNCHANGED: '',
};

const STATUS_CLASS: Record<DiffStatus, string> = {
  NEW: 'pf-hde-status-text pf-hde-status-text--new',
  REMOVED: 'pf-hde-status-text pf-hde-status-text--removed',
  GREW: 'pf-hde-status-text pf-hde-status-text--grew',
  SHRANK: 'pf-hde-status-text pf-hde-status-text--shrank',
  UNCHANGED: 'pf-hde-status-text pf-hde-status-text--unchanged',
};

export const statusRenderer: CellRenderer = (
  value: SqlValue,
): CellRenderResult => {
  const s = String(value ?? 'UNCHANGED') as DiffStatus;
  const label = STATUS_LABEL[s] ?? s;
  if (label === '') {
    return {content: m('span'), align: 'left'};
  }
  return {
    content: m(
      'span',
      {
        'class': STATUS_CLASS[s] ?? 'pf-hde-status-text',
        'aria-label': `Status: ${label}`,
      },
      label,
    ),
    align: 'left',
  };
};

export const deltaSizeRenderer: CellRenderer = (
  value: SqlValue,
): CellRenderResult => {
  return renderDelta(value, (n) => fmtSize(Number(n)));
};

export const deltaCountRenderer: CellRenderer = (
  value: SqlValue,
): CellRenderResult => {
  return renderDelta(value, (n) => Math.abs(Number(n)).toLocaleString());
};

function renderDelta(
  value: SqlValue,
  fmtMagnitude: (n: Num) => string,
): CellRenderResult {
  const n = toNum(value);
  if (n == null) {
    return {
      content: m('span', {class: 'pf-hde-mono pf-hde-muted'}, '—'),
      align: 'right',
    };
  }
  const sign = compareToZero(n);
  if (sign === 0) {
    return {
      content: m(
        'span',
        {'class': 'pf-hde-mono pf-hde-delta--zero', 'aria-label': 'No change'},
        '0',
      ),
      align: 'right',
    };
  }
  const symbol = sign > 0 ? '+' : '−';
  const cls =
    sign > 0
      ? 'pf-hde-mono pf-hde-delta--grew'
      : 'pf-hde-mono pf-hde-delta--shrank';
  const word = sign > 0 ? 'increased by' : 'decreased by';
  const magnitude = fmtMagnitude(absNum(n));
  return {
    content: m(
      'span',
      {'class': cls, 'aria-label': `${word} ${magnitude}`},
      `${symbol}${magnitude}`,
    ),
    align: 'right',
  };
}

// null → "—" so empty cells are visible (the column has data on the
// other side).
export const sideSizeRenderer: CellRenderer = (
  value: SqlValue,
): CellRenderResult => {
  const n = toNum(value);
  if (n == null) {
    return {
      content: m('span', {class: 'pf-hde-mono pf-hde-muted'}, '—'),
      align: 'right',
    };
  }
  return {
    content: m('span', {class: 'pf-hde-mono'}, fmtSize(Number(n))),
    align: 'right',
  };
};

export const sideCountRenderer: CellRenderer = (
  value: SqlValue,
): CellRenderResult => {
  const n = toNum(value);
  if (n == null) {
    return {
      content: m('span', {class: 'pf-hde-mono pf-hde-muted'}, '—'),
      align: 'right',
    };
  }
  return {
    content: m('span', {class: 'pf-hde-mono'}, Number(n).toLocaleString()),
    align: 'right',
  };
};

function toNum(v: SqlValue): Num | null {
  if (v == null) return null;
  if (typeof v === 'number' || typeof v === 'bigint') return v;
  if (typeof v === 'string') {
    const n = Number(v);
    if (!Number.isNaN(n)) return n;
  }
  return null;
}

function compareToZero(n: Num): number {
  if (typeof n === 'bigint') return n < 0n ? -1 : n > 0n ? 1 : 0;
  return n < 0 ? -1 : n > 0 ? 1 : 0;
}

function absNum(n: Num): Num {
  if (typeof n === 'bigint') return n < 0n ? -n : n;
  return Math.abs(n);
}

// `field` is the raw column name in the merged DiffRow; `title` is the
// user-facing label.
export interface DiffNumericField {
  readonly field: string;
  readonly title: string;
  readonly kind: 'size' | 'count';
}

export function buildSizeCountSchema(opts: {
  readonly keyTitle: string;
  readonly keyRenderer?: CellRenderer;
  readonly size: DiffNumericField;
  readonly count: DiffNumericField;
  readonly extraFields?: ReadonlyArray<DiffNumericField>;
}): SchemaRegistry {
  const cols: Record<string, ColumnDef> = {
    [KEY_COL]: {
      title: opts.keyTitle,
      columnType: 'text',
      cellRenderer: opts.keyRenderer,
    },
    [STATUS_COL]: {
      title: 'Status',
      columnType: 'text',
      cellRenderer: statusRenderer,
    },
  };
  for (const ef of [opts.size, opts.count, ...(opts.extraFields ?? [])]) {
    const sideRenderer =
      ef.kind === 'size' ? sideSizeRenderer : sideCountRenderer;
    const dRenderer =
      ef.kind === 'size' ? deltaSizeRenderer : deltaCountRenderer;
    cols[deltaCol(ef.field)] = {
      title: `Δ ${ef.title}`,
      columnType: 'quantitative',
      cellRenderer: dRenderer,
    };
    cols[baselineCol(ef.field)] = {
      title: `Baseline ${ef.title}`,
      columnType: 'quantitative',
      cellRenderer: sideRenderer,
    };
    cols[currentCol(ef.field)] = {
      title: `Current ${ef.title}`,
      columnType: 'quantitative',
      cellRenderer: sideRenderer,
    };
  }
  return {query: cols};
}

// Default sort: |Δ size| desc — biggest mover first.
export function buildSizeCountInitialColumns(opts: {
  readonly size: DiffNumericField;
  readonly count: DiffNumericField;
  readonly extraFields?: ReadonlyArray<DiffNumericField>;
}): Array<{id: string; field: string; sort?: 'ASC' | 'DESC'}> {
  const cols: Array<{id: string; field: string; sort?: 'ASC' | 'DESC'}> = [
    {id: KEY_COL, field: KEY_COL},
    {id: STATUS_COL, field: STATUS_COL},
  ];
  let sortApplied = false;
  for (const ef of [opts.size, opts.count, ...(opts.extraFields ?? [])]) {
    const isPrimarySize = ef === opts.size;
    cols.push({
      id: deltaCol(ef.field),
      field: deltaCol(ef.field),
      sort: !sortApplied && isPrimarySize ? 'DESC' : undefined,
    });
    if (!sortApplied && isPrimarySize) sortApplied = true;
    cols.push({id: baselineCol(ef.field), field: baselineCol(ef.field)});
    cols.push({id: currentCol(ef.field), field: currentCol(ef.field)});
  }
  return cols;
}
