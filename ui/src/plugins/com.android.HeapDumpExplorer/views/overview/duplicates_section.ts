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

// Duplicate-Bitmaps / Strings / Arrays cards on the Overview tab. Single
// engine: a flat DataGrid. Diff: outer-join on the duplicate key with
// Baseline / Current / Δ columns.

import m from 'mithril';
import type {SqlValue, Row} from '../../../../trace_processor/query_result';
import {DataGrid} from '../../../../components/widgets/datagrid/datagrid';
import type {
  CellRenderer,
  CellRenderResult,
  ColumnDef,
  SchemaRegistry,
} from '../../../../components/widgets/datagrid/datagrid_schema';
import type {OverviewData} from '../../types';
import {fmtSize} from '../../format';
import type {NavState} from '../../nav_state';
import {type NavFn, sizeRenderer} from '../../components';
import {
  baselineCol,
  currentCol,
  dedupeByKey,
  deltaCol,
  mergeRows,
  KEY_COL,
  STATUS_COL,
} from '../../diff/diff_rows';
import {
  deltaCountRenderer,
  deltaSizeRenderer,
  sideCountRenderer,
  sideSizeRenderer,
  statusRenderer,
} from '../../diff/diff_schemas';

export function renderDuplicateBitmapsCard(
  overview: OverviewData,
  baselineOverview: OverviewData | undefined,
  navigate: NavFn,
): m.Children {
  const cur = overview.duplicateBitmaps ?? [];
  const base = baselineOverview?.duplicateBitmaps ?? [];
  if (cur.length === 0 && base.length === 0 && !overview.hasFieldValues) {
    return null;
  }
  if (cur.length === 0 && base.length === 0) {
    return m(
      'div',
      {class: 'pf-hde-card pf-hde-mb-4'},
      m('p', {class: 'pf-hde-muted'}, 'No duplicate bitmaps found.'),
    );
  }
  const isDiff = baselineOverview !== undefined;
  const summary = makeSummary('group', cur, base, (g) => g.wastedBytes, isDiff);

  if (!isDiff) {
    return renderDuplicateSectionSingle({
      title: 'Duplicate Bitmaps',
      summary,
      targetView: 'bitmaps',
      linkLabel: 'View Bitmaps',
      navigate,
      data: cur.map((g) => ({
        dimensions: `${g.width} × ${g.height}`,
        groupKey: g.groupKey,
        copies: g.count,
        total_bytes: g.totalBytes,
        wasted_bytes: g.wastedBytes,
      })),
      schema: {
        query: {
          dimensions: {title: 'Dimensions', columnType: 'text'},
          groupKey: {title: 'Hash', columnType: 'text'},
          copies: {
            title: 'Copies',
            columnType: 'quantitative',
            cellRenderer: makeNavCountRenderer((row) =>
              navigate('bitmaps', {filterKey: String(row.groupKey ?? '')}),
            ),
          },
          total_bytes: {
            title: 'Total',
            columnType: 'quantitative',
            cellRenderer: sizeRenderer,
          },
          wasted_bytes: {
            title: 'Wasted',
            columnType: 'quantitative',
            cellRenderer: sizeRenderer,
          },
        },
      },
      initialColumns: [
        {id: 'dimensions', field: 'dimensions'},
        {id: 'groupKey', field: 'groupKey'},
        {id: 'copies', field: 'copies'},
        {id: 'total_bytes', field: 'total_bytes'},
        {id: 'wasted_bytes', field: 'wasted_bytes'},
      ],
    });
  }

  const numericFields = ['copies', 'total_bytes', 'wasted_bytes'];
  const baseRows = dedupeByKey(
    base.map((g) => ({
      key: g.groupKey,
      dimensions: `${g.width} × ${g.height}`,
      copies: g.count,
      total_bytes: g.totalBytes,
      wasted_bytes: g.wastedBytes,
    })),
    (r) => String(r.key),
    numericFields,
  );
  const curRows = dedupeByKey(
    cur.map((g) => ({
      key: g.groupKey,
      dimensions: `${g.width} × ${g.height}`,
      copies: g.count,
      total_bytes: g.totalBytes,
      wasted_bytes: g.wastedBytes,
    })),
    (r) => String(r.key),
    numericFields,
  );
  const merged = mergeRows({
    baseline: baseRows,
    current: curRows,
    keyOf: (r) => String(r.key),
    numericFields,
    passThroughFields: ['dimensions'],
    primaryDeltaField: 'wasted_bytes',
  });
  return renderDuplicateSectionDiff({
    title: 'Duplicate Bitmaps',
    summary,
    targetView: 'bitmaps',
    linkLabel: 'View Bitmaps diff',
    navigate,
    data: merged.map((r) => ({...r, groupKey: r[KEY_COL]})),
    keyTitle: 'Hash',
    keyField: 'groupKey',
    extraTextFields: [{field: 'dimensions', title: 'Dimensions'}],
    sizeFields: [
      {field: 'wasted_bytes', title: 'Wasted'},
      {field: 'total_bytes', title: 'Total'},
    ],
    countFields: [{field: 'copies', title: 'Copies'}],
    primarySortField: 'wasted_bytes',
  });
}

export function renderDuplicateStringsCard(
  overview: OverviewData,
  baselineOverview: OverviewData | undefined,
  navigate: NavFn,
): m.Children {
  const cur = overview.duplicateStrings ?? [];
  const base = baselineOverview?.duplicateStrings ?? [];
  if (cur.length === 0 && base.length === 0 && !overview.hasFieldValues) {
    return null;
  }
  if (cur.length === 0 && base.length === 0) {
    return m(
      'div',
      {class: 'pf-hde-card pf-hde-mb-4'},
      m('p', {class: 'pf-hde-muted'}, 'No duplicate strings found.'),
    );
  }
  const isDiff = baselineOverview !== undefined;
  const summary = makeSummary('group', cur, base, (g) => g.wastedBytes, isDiff);

  if (!isDiff) {
    return renderDuplicateSectionSingle({
      title: 'Duplicate Strings',
      summary,
      targetView: 'strings',
      linkLabel: 'View Strings',
      navigate,
      data: cur.map((g) => ({
        value: g.value,
        copies: g.count,
        total_bytes: g.totalBytes,
        wasted_bytes: g.wastedBytes,
      })),
      schema: {
        query: {
          value: {
            title: 'Value',
            columnType: 'text',
            cellRenderer: makeStringRenderer((row) =>
              navigate('strings', {q: String(row.value ?? '')}),
            ),
          },
          copies: {
            title: 'Copies',
            columnType: 'quantitative',
            cellRenderer: makeNavCountRenderer((row) =>
              navigate('strings', {q: String(row.value ?? '')}),
            ),
          },
          total_bytes: {
            title: 'Total',
            columnType: 'quantitative',
            cellRenderer: sizeRenderer,
          },
          wasted_bytes: {
            title: 'Wasted',
            columnType: 'quantitative',
            cellRenderer: sizeRenderer,
          },
        },
      },
      initialColumns: [
        {id: 'value', field: 'value'},
        {id: 'copies', field: 'copies'},
        {id: 'total_bytes', field: 'total_bytes'},
        {id: 'wasted_bytes', field: 'wasted_bytes'},
      ],
    });
  }

  const numericFields = ['copies', 'total_bytes', 'wasted_bytes'];
  const baseRows = dedupeByKey(
    base.map((g) => ({
      key: g.value,
      copies: g.count,
      total_bytes: g.totalBytes,
      wasted_bytes: g.wastedBytes,
    })),
    (r) => String(r.key),
    numericFields,
  );
  const curRows = dedupeByKey(
    cur.map((g) => ({
      key: g.value,
      copies: g.count,
      total_bytes: g.totalBytes,
      wasted_bytes: g.wastedBytes,
    })),
    (r) => String(r.key),
    numericFields,
  );
  const merged = mergeRows({
    baseline: baseRows,
    current: curRows,
    keyOf: (r) => String(r.key),
    numericFields,
    primaryDeltaField: 'wasted_bytes',
  });
  return renderDuplicateSectionDiff({
    title: 'Duplicate Strings',
    summary,
    targetView: 'strings',
    linkLabel: 'View Strings diff',
    navigate,
    data: merged.map((r) => ({...r, value: r[KEY_COL]})),
    keyTitle: 'Value',
    keyField: 'value',
    keyRenderer: makeStringRenderer((row) =>
      navigate('strings', {q: String(row.value ?? '')}),
    ),
    extraTextFields: [],
    sizeFields: [
      {field: 'wasted_bytes', title: 'Wasted'},
      {field: 'total_bytes', title: 'Total'},
    ],
    countFields: [{field: 'copies', title: 'Copies'}],
    primarySortField: 'wasted_bytes',
  });
}

export function renderDuplicateArraysCard(
  overview: OverviewData,
  baselineOverview: OverviewData | undefined,
  navigate: NavFn,
): m.Children {
  const cur = overview.duplicateArrays ?? [];
  const base = baselineOverview?.duplicateArrays ?? [];
  if (cur.length === 0 && base.length === 0) return null;
  const isDiff = baselineOverview !== undefined;
  const summary = makeSummary('group', cur, base, (g) => g.wastedBytes, isDiff);

  if (!isDiff) {
    return renderDuplicateSectionSingle({
      title: 'Duplicate Primitive Arrays',
      summary,
      targetView: 'arrays',
      linkLabel: 'View Arrays',
      navigate,
      data: cur.map((g) => ({
        className: g.className,
        arrayHash: g.arrayHash,
        copies: g.count,
        total_bytes: g.totalBytes,
        wasted_bytes: g.wastedBytes,
      })),
      schema: {
        query: {
          className: {
            title: 'Array Type',
            columnType: 'text',
            cellRenderer: (value: SqlValue) =>
              ({
                content: m(
                  'button',
                  {
                    class: 'pf-hde-link',
                    onclick: () =>
                      navigate('objects', {cls: String(value ?? '')}),
                  },
                  String(value ?? ''),
                ),
                align: 'left',
              }) as CellRenderResult,
          },
          arrayHash: {title: 'Hash', columnType: 'text'},
          copies: {
            title: 'Copies',
            columnType: 'quantitative',
            cellRenderer: makeNavCountRenderer((row) =>
              navigate('arrays', {arrayHash: String(row.arrayHash ?? '')}),
            ),
          },
          total_bytes: {
            title: 'Total',
            columnType: 'quantitative',
            cellRenderer: sizeRenderer,
          },
          wasted_bytes: {
            title: 'Wasted',
            columnType: 'quantitative',
            cellRenderer: sizeRenderer,
          },
        },
      },
      initialColumns: [
        {id: 'className', field: 'className'},
        {id: 'arrayHash', field: 'arrayHash'},
        {id: 'copies', field: 'copies'},
        {id: 'total_bytes', field: 'total_bytes'},
        {id: 'wasted_bytes', field: 'wasted_bytes'},
      ],
    });
  }

  const numericFields = ['copies', 'total_bytes', 'wasted_bytes'];
  const baseRows = dedupeByKey(
    base.map((g) => ({
      key: g.arrayHash,
      className: g.className,
      copies: g.count,
      total_bytes: g.totalBytes,
      wasted_bytes: g.wastedBytes,
    })),
    (r) => String(r.key),
    numericFields,
  );
  const curRows = dedupeByKey(
    cur.map((g) => ({
      key: g.arrayHash,
      className: g.className,
      copies: g.count,
      total_bytes: g.totalBytes,
      wasted_bytes: g.wastedBytes,
    })),
    (r) => String(r.key),
    numericFields,
  );
  const merged = mergeRows({
    baseline: baseRows,
    current: curRows,
    keyOf: (r) => String(r.key),
    numericFields,
    passThroughFields: ['className'],
    primaryDeltaField: 'wasted_bytes',
  });
  return renderDuplicateSectionDiff({
    title: 'Duplicate Primitive Arrays',
    summary,
    targetView: 'arrays',
    linkLabel: 'View Arrays diff',
    navigate,
    data: merged.map((r) => ({...r, arrayHash: r[KEY_COL]})),
    keyTitle: 'Hash',
    keyField: 'arrayHash',
    extraTextFields: [{field: 'className', title: 'Array Type'}],
    sizeFields: [
      {field: 'wasted_bytes', title: 'Wasted'},
      {field: 'total_bytes', title: 'Total'},
    ],
    countFields: [{field: 'copies', title: 'Copies'}],
    primarySortField: 'wasted_bytes',
  });
}

// ----- Shared helpers ------------------------------------------------------

interface SingleSectionOpts {
  readonly title: string;
  readonly summary: m.Children;
  readonly targetView: string;
  readonly linkLabel: string;
  readonly navigate: NavFn;
  readonly data: Row[];
  readonly schema: SchemaRegistry;
  readonly initialColumns: Array<{id: string; field: string}>;
}

function renderDuplicateSectionSingle(opts: SingleSectionOpts): m.Children {
  return m('div', {class: 'pf-hde-card pf-hde-mb-4'}, [
    m('h3', {class: 'pf-hde-sub-heading'}, opts.title),
    m('p', {class: 'pf-hde-desc'}, [
      opts.summary,
      ' ',
      m(
        'button',
        {
          class: 'pf-hde-link--alt',
          onclick: () => opts.navigate(opts.targetView as NavState['view']),
        },
        opts.linkLabel,
      ),
    ]),
    m(DataGrid, {
      schema: opts.schema,
      rootSchema: 'query',
      data: opts.data,
      initialColumns: opts.initialColumns,
    }),
  ]);
}

interface DiffSectionOpts {
  readonly title: string;
  readonly summary: m.Children;
  readonly targetView: string;
  readonly linkLabel: string;
  readonly navigate: NavFn;
  readonly data: Row[];
  readonly keyTitle: string;
  readonly keyField: string;
  readonly keyRenderer?: CellRenderer;
  readonly extraTextFields: ReadonlyArray<{field: string; title: string}>;
  readonly sizeFields: ReadonlyArray<{field: string; title: string}>;
  readonly countFields: ReadonlyArray<{field: string; title: string}>;
  /** Numeric field used for default sort by `|Δ|` desc. */
  readonly primarySortField: string;
}

function renderDuplicateSectionDiff(opts: DiffSectionOpts): m.Children {
  const cols: Record<string, ColumnDef> = {
    [opts.keyField]: {
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
  for (const tf of opts.extraTextFields) {
    cols[tf.field] = {title: tf.title, columnType: 'text'};
  }
  for (const f of opts.sizeFields) {
    cols[deltaCol(f.field)] = {
      title: 'Δ ' + f.title,
      columnType: 'quantitative',
      cellRenderer: deltaSizeRenderer,
    };
    cols[baselineCol(f.field)] = {
      title: 'Baseline ' + f.title,
      columnType: 'quantitative',
      cellRenderer: sideSizeRenderer,
    };
    cols[currentCol(f.field)] = {
      title: 'Current ' + f.title,
      columnType: 'quantitative',
      cellRenderer: sideSizeRenderer,
    };
  }
  for (const f of opts.countFields) {
    cols[deltaCol(f.field)] = {
      title: 'Δ ' + f.title,
      columnType: 'quantitative',
      cellRenderer: deltaCountRenderer,
    };
    cols[baselineCol(f.field)] = {
      title: 'Baseline ' + f.title,
      columnType: 'quantitative',
      cellRenderer: sideCountRenderer,
    };
    cols[currentCol(f.field)] = {
      title: 'Current ' + f.title,
      columnType: 'quantitative',
      cellRenderer: sideCountRenderer,
    };
  }
  const initialColumns: Array<{
    id: string;
    field: string;
    sort?: 'ASC' | 'DESC';
  }> = [
    {id: opts.keyField, field: opts.keyField},
    {id: STATUS_COL, field: STATUS_COL},
  ];
  for (const tf of opts.extraTextFields) {
    initialColumns.push({id: tf.field, field: tf.field});
  }
  for (const f of [...opts.sizeFields, ...opts.countFields]) {
    if (f.field === opts.primarySortField) {
      initialColumns.push({
        id: deltaCol(f.field),
        field: deltaCol(f.field),
        sort: 'DESC',
      });
    } else {
      initialColumns.push({id: deltaCol(f.field), field: deltaCol(f.field)});
    }
    initialColumns.push({
      id: baselineCol(f.field),
      field: baselineCol(f.field),
    });
    initialColumns.push({id: currentCol(f.field), field: currentCol(f.field)});
  }
  return m('div', {class: 'pf-hde-card pf-hde-mb-4'}, [
    m('h3', {class: 'pf-hde-sub-heading'}, opts.title),
    m('p', {class: 'pf-hde-desc'}, [
      opts.summary,
      ' ',
      m(
        'button',
        {
          class: 'pf-hde-link--alt',
          onclick: () => opts.navigate(opts.targetView as NavState['view']),
        },
        opts.linkLabel,
      ),
    ]),
    m(DataGrid, {
      schema: {query: cols},
      rootSchema: 'query',
      data: opts.data,
      initialColumns,
    }),
  ]);
}

function makeNavCountRenderer(onclick: (row: Row) => void): CellRenderer {
  return (value: SqlValue, row: Row): CellRenderResult => ({
    content: m(
      'button',
      {class: 'pf-hde-link', onclick: () => onclick(row)},
      String(value ?? '0'),
    ),
    align: 'right',
  });
}

function makeStringRenderer(onclick: (row: Row) => void): CellRenderer {
  return (value: SqlValue, row: Row): CellRenderResult => {
    const s = String(value ?? '');
    const display = s.length > 200 ? s.slice(0, 200) + '…' : s;
    return {
      content: m(
        'button',
        {
          class: 'pf-hde-link pf-hde-mono pf-hde-break-all pf-hde-str-color',
          onclick: () => onclick(row),
        },
        '"' + display + '"',
      ),
      align: 'left',
    };
  };
}

interface DuplicateGroupLike {
  readonly wastedBytes: number;
}

function makeSummary<T extends DuplicateGroupLike>(
  unit: string,
  cur: ReadonlyArray<T>,
  base: ReadonlyArray<T>,
  wastedBytes: (g: T) => number,
  isDiff: boolean,
): m.Children {
  const cWasted = cur.reduce((a, g) => a + wastedBytes(g), 0);
  if (!isDiff) {
    return [
      cur.length +
        ' ' +
        unit +
        (cur.length !== 1 ? 's' : '') +
        ' detected, wasting ',
      m('span', {class: 'pf-hde-mono pf-hde-semibold'}, fmtSize(cWasted)),
      '.',
    ];
  }
  const bWasted = base.reduce((a, g) => a + wastedBytes(g), 0);
  const dWasted = cWasted - bWasted;
  const dGroups = cur.length - base.length;
  return [
    `${cur.length} ${unit}${cur.length !== 1 ? 's' : ''} `,
    m('span', {class: 'pf-hde-mono'}, `(${dGroups >= 0 ? '+' : ''}${dGroups})`),
    ', wasting ',
    m('span', {class: 'pf-hde-mono pf-hde-semibold'}, fmtSize(cWasted)),
    ' ',
    m(
      'span',
      {
        class:
          'pf-hde-mono ' +
          (dWasted > 0
            ? 'pf-hde-delta--grew'
            : dWasted < 0
              ? 'pf-hde-delta--shrank'
              : 'pf-hde-delta--zero'),
      },
      `(${dWasted >= 0 ? '+' : '−'}${fmtSize(Math.abs(dWasted))})`,
    ),
    '.',
  ];
}
