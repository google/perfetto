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

// Overview tab. Five DataGrid cards. When `baselineOverview` is present
// each card gains Baseline / Current / Δ columns merged via mergeRows.

import m from 'mithril';
import {Duration} from '../../../base/time';
import type {SqlValue, Row} from '../../../trace_processor/query_result';
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import type {
  CellRenderer,
  CellRenderResult,
  ColumnDef,
  SchemaRegistry,
} from '../../../components/widgets/datagrid/datagrid_schema';
import {Callout} from '../../../widgets/callout';
import {Intent} from '../../../widgets/common';
import type {OverviewData, HeapInfo} from '../types';
import {fmtSize} from '../format';
import {type NavFn, sizeRenderer, countRenderer} from '../components';
import type {HeapDump} from '../queries';
import {getLoadState} from '../baseline/load_action';
import {openBaselineFilePicker, shouldShowBaselineHeader} from '../header';
import {Button, ButtonVariant} from '../../../widgets/button';
import {
  baselineCol,
  currentCol,
  deltaCol,
  mergeRows,
  KEY_COL,
  STATUS_COL,
} from '../diff/diff_rows';
import {
  deltaCountRenderer,
  deltaSizeRenderer,
  sideSizeRenderer,
  statusRenderer,
} from '../diff/diff_schemas';
import {
  renderDuplicateArraysCard,
  renderDuplicateBitmapsCard,
  renderDuplicateStringsCard,
} from './overview/duplicates_section';

export const HIDE_DEFAULT_CHANGED_KEY =
  'hideHeapDumpExplorerDefaultChangedHint';

interface OverviewViewAttrs {
  readonly overview: OverviewData;
  readonly activeDump: HeapDump;
  /**
   * True when a baseline is selected (mode is diff). The heading flips even
   * before the baseline overview query resolves so the user sees that the
   * page is in diff mode immediately.
   */
  readonly diffActive: boolean;
  /** When present → render diff columns alongside current values. */
  readonly baselineOverview?: OverviewData;
  /**
   * True when a baseline is loaded but its overview query is still running
   * (so diff columns are coming, not absent).
   */
  readonly baselineLoading?: boolean;
  readonly navigate: NavFn;
  readonly showDefaultChangedHint: boolean;
  readonly onBackToTimeline: () => void;
  readonly onDismissDefaultChangedHint: () => void;
}

function OverviewView(): m.Component<OverviewViewAttrs> {
  return {
    view(vnode) {
      const {
        overview,
        activeDump,
        diffActive,
        baselineOverview,
        baselineLoading,
        navigate,
        showDefaultChangedHint,
        onBackToTimeline,
        onDismissDefaultChangedHint,
      } = vnode.attrs;
      const isDiff = baselineOverview !== undefined;
      const heading = diffActive ? 'Overview diff' : 'Overview';
      // Mode tag baked into every card's vnode key. DataGrid captures its
      // `initialColumns` only on `oninit`, so we must force a remount when
      // we flip between the single-engine and diff column sets.
      const mode = isDiff ? 'diff' : 'single';

      // Mithril requires sibling vnodes in a fragment to either all have
      // keys or none. Wrap each top-level child in a keyed div so we can
      // freely use keys on individual cards (force-remount on mode flip)
      // without triggering "all-or-none keys" runtime errors.
      const child = (key: string, content: m.Children): m.Vnode =>
        m('div', {key}, content);

      return m('div', {class: 'pf-hde-view-scroll'}, [
        child('heading', m('h2', {class: 'pf-hde-view-heading'}, heading)),
        child(
          'default-changed-hint',
          showDefaultChangedHint
            ? m(
                Callout,
                {
                  className: 'pf-hde-default-changed-callout pf-hde-mb-4',
                  icon: 'info',
                  dismissible: true,
                  onDismiss: onDismissDefaultChangedHint,
                },
                m('p', [
                  m(
                    'span',
                    'Heapdump Explorer is now the default view for traces ' +
                      'with heap-graph data. ',
                  ),
                  m(Button, {
                    label: 'Back to Timeline',
                    icon: 'arrow_back',
                    compact: true,
                    onclick: onBackToTimeline,
                  }),
                ]),
              )
            : null,
        ),
        child('load', renderLoadBaselineSection()),
        child(
          'loading',
          baselineLoading === true && !isDiff
            ? m(
                Callout,
                {
                  icon: 'hourglass_empty',
                  intent: Intent.None,
                  className: 'pf-hde-mb-4',
                  role: 'status',
                },
                'Computing baseline overview… diff columns will appear once it finishes.',
              )
            : null,
        ),
        child(
          `info-${mode}`,
          renderInfoCard(activeDump, overview, baselineOverview),
        ),
        child(`heaps-${mode}`, renderHeapsCard(overview, baselineOverview)),
        child(
          `bitmaps-${mode}`,
          renderDuplicateBitmapsCard(overview, baselineOverview, navigate),
        ),
        child(
          `strings-${mode}`,
          renderDuplicateStringsCard(overview, baselineOverview, navigate),
        ),
        child(
          `arrays-${mode}`,
          renderDuplicateArraysCard(overview, baselineOverview, navigate),
        ),
      ]);
    },
  };
}

// ----- Top-of-tab "Load baseline" affordance -------------------------------
//
// Only rendered in single-engine mode. When a baseline IS loaded, the slim
// header above the tabs holds the controls — no need to repeat them here.

function renderLoadBaselineSection(): m.Children {
  // The Overview-tab CTA is the discovery entry point for diff mode in the
  // common single-trace, no-diff workflow. Once the top bar is showing
  // baseline state (a load is in flight, an error needs reading, or a
  // pool / active baseline exists) the row's selector takes over and the
  // CTA collapses to keep the page free of duplicated affordances.
  if (shouldShowBaselineHeader()) return null;
  const {error} = getLoadState();
  // Bare button + helper text; we deliberately don't wrap in a Callout
  // here because the Callout's leading icon collides visually with the
  // button's `difference` icon (two adjacent glyphs reading the same).
  return [
    m(
      'div',
      {class: 'pf-hde-heading-row pf-hde-mb-4'},
      m(Button, {
        label: 'Diff against another trace…',
        icon: 'difference',
        intent: Intent.Primary,
        variant: ButtonVariant.Filled,
        onclick: () => openBaselineFilePicker(),
      }),
    ),
    error &&
      m(
        Callout,
        {
          icon: 'error',
          intent: Intent.Danger,
          role: 'alert',
          className: 'pf-hde-mb-4',
        },
        error,
      ),
  ];
}

// ----- General Information --------------------------------------------------

function renderInfoCard(
  activeDump: HeapDump,
  overview: OverviewData,
  baselineOverview: OverviewData | undefined,
): m.Children {
  const processLabel =
    (activeDump.processName ?? '<unknown>') +
    (activeDump.pid !== null ? ` (pid ${activeDump.pid})` : '');
  if (baselineOverview === undefined) {
    const rows: Row[] = [
      {property: 'Process', value: processLabel},
      ...(overview.processUptime !== null
        ? [{property: 'Uptime', value: Duration.format(overview.processUptime)}]
        : []),
      ...(overview.oomBucket !== null
        ? [
            {
              property: 'OOM score',
              value: `${overview.oomBucket} (${overview.oomScore})`,
            },
          ]
        : []),
      {property: 'Classes', value: overview.classCount.toLocaleString()},
      {
        property: 'Reachable instances',
        value: overview.reachableInstanceCount.toLocaleString(),
      },
      {
        property: 'Unreachable instances',
        value: overview.unreachableInstanceCount.toLocaleString(),
      },
      ...(overview.anonRssAndSwapSize !== null
        ? [
            {
              property: 'Anon RSS + Swap',
              value: fmtSize(Number(overview.anonRssAndSwapSize)),
            },
          ]
        : []),
      ...(overview.dmabufRssSize !== null
        ? [
            {
              property: 'DMA Buffer RSS',
              value: fmtSize(Number(overview.dmabufRssSize)),
            },
          ]
        : []),
      {
        property: 'Heaps',
        value: overview.heaps.map((h) => h.name).join(', '),
      },
    ];
    const schema: SchemaRegistry = {
      query: {
        property: {title: 'Property', columnType: 'text'},
        value: {title: 'Value', columnType: 'text'},
      },
    };
    return m('div', {class: 'pf-hde-card pf-hde-mb-4'}, [
      m('h3', {class: 'pf-hde-sub-heading'}, 'General Information'),
      m(DataGrid, {
        schema,
        rootSchema: 'query',
        data: rows,
        initialColumns: [
          {id: 'property', field: 'property'},
          {id: 'value', field: 'value'},
        ],
      }),
    ]);
  }

  const rows: Row[] = [
    {
      property: 'Reachable instances',
      baseline: baselineOverview.reachableInstanceCount,
      current: overview.reachableInstanceCount,
      delta:
        overview.reachableInstanceCount -
        baselineOverview.reachableInstanceCount,
    },
    {
      property: 'Heaps',
      baseline: baselineOverview.heaps.map((h) => h.name).join(', '),
      current: overview.heaps.map((h) => h.name).join(', '),
      delta: heapDeltaSummary(overview.heaps, baselineOverview.heaps),
    },
  ];
  const schema: SchemaRegistry = {
    query: {
      property: {title: 'Property', columnType: 'text'},
      baseline: {
        title: 'Baseline',
        columnType: 'text',
        cellRenderer: maybeNumericRenderer,
      },
      current: {
        title: 'Current',
        columnType: 'text',
        cellRenderer: maybeNumericRenderer,
      },
      delta: {
        title: 'Δ',
        columnType: 'text',
        cellRenderer: maybeDeltaCountRenderer,
      },
    },
  };
  return m('div', {class: 'pf-hde-card pf-hde-mb-4'}, [
    m('h3', {class: 'pf-hde-sub-heading'}, 'General Information'),
    m(DataGrid, {
      schema,
      rootSchema: 'query',
      data: rows,
      initialColumns: [
        {id: 'property', field: 'property'},
        {id: 'baseline', field: 'baseline'},
        {id: 'current', field: 'current'},
        {id: 'delta', field: 'delta'},
      ],
    }),
  ]);
}

const maybeNumericRenderer: CellRenderer = (value: SqlValue) => {
  if (typeof value === 'number' || typeof value === 'bigint') {
    return countRenderer(value);
  }
  return {
    content: m('span', String(value ?? '')),
    align: 'left',
  } satisfies CellRenderResult;
};

const maybeDeltaCountRenderer: CellRenderer = (value: SqlValue, row: Row) => {
  if (typeof value === 'number' || typeof value === 'bigint') {
    return deltaCountRenderer(value, row);
  }
  return {
    content: m('span', {class: 'pf-hde-muted'}, String(value ?? '')),
    align: 'left',
  } satisfies CellRenderResult;
};

function heapDeltaSummary(current: HeapInfo[], baseline: HeapInfo[]): string {
  const cSet = new Set(current.map((h) => h.name));
  const bSet = new Set(baseline.map((h) => h.name));
  const added = [...cSet].filter((h) => !bSet.has(h));
  const removed = [...bSet].filter((h) => !cSet.has(h));
  if (added.length === 0 && removed.length === 0) return 'same';
  const parts: string[] = [];
  if (added.length) parts.push(`+${added.join(', ')}`);
  if (removed.length) parts.push(`−${removed.join(', ')}`);
  return parts.join('; ');
}

// ----- Bytes retained by heap ----------------------------------------------

function renderHeapsCard(
  overview: OverviewData,
  baselineOverview: OverviewData | undefined,
): m.Children {
  // Only show heaps with non-zero retention on at least one side.
  const filterNonZero = (heaps: HeapInfo[]) =>
    heaps.filter((h) => h.java + h.native_ > 0);
  const cHeaps = filterNonZero(overview.heaps);

  if (baselineOverview === undefined) {
    const rows: Row[] = withTotalRow(
      cHeaps.map((h) => ({
        heap: h.name,
        java_size: h.java,
        native_size: h.native_,
        total_size: h.java + h.native_,
      })),
      'heap',
    );
    return m('div', {class: 'pf-hde-card pf-hde-mb-4'}, [
      m('h3', {class: 'pf-hde-sub-heading'}, 'Bytes Retained by Heap'),
      m(DataGrid, {
        schema: {
          query: {
            heap: {title: 'Heap', columnType: 'text'},
            java_size: {
              title: 'Java',
              columnType: 'quantitative',
              cellRenderer: sizeRenderer,
            },
            native_size: {
              title: 'Native',
              columnType: 'quantitative',
              cellRenderer: sizeRenderer,
            },
            total_size: {
              title: 'Total',
              columnType: 'quantitative',
              cellRenderer: sizeRenderer,
            },
          },
        },
        rootSchema: 'query',
        data: rows,
        initialColumns: [
          {id: 'heap', field: 'heap'},
          {id: 'java_size', field: 'java_size'},
          {id: 'native_size', field: 'native_size'},
          {id: 'total_size', field: 'total_size'},
        ],
      }),
    ]);
  }

  const bHeaps = filterNonZero(baselineOverview.heaps);
  const merged = mergeRows({
    baseline: bHeaps.map((h) => ({
      heap: h.name,
      java_size: h.java,
      native_size: h.native_,
      total_size: h.java + h.native_,
    })),
    current: cHeaps.map((h) => ({
      heap: h.name,
      java_size: h.java,
      native_size: h.native_,
      total_size: h.java + h.native_,
    })),
    keyOf: (r) => String(r.heap ?? ''),
    numericFields: ['java_size', 'native_size', 'total_size'],
    primaryDeltaField: 'total_size',
  });
  // Rename the merged-row 'key' column to 'heap' for the schema, and pin
  // a synthesized Total row at the top — same rollup the non-diff card
  // shows, kept here so users don't lose the bottom-line view in diff mode.
  const totalRow = sumDiffRows(merged, 'Total', [
    'java_size',
    'native_size',
    'total_size',
  ]);
  const dataRows: Row[] = [totalRow, ...merged].map((r) => ({
    ...r,
    heap: r[KEY_COL],
  }));
  const schema = buildHeapsDiffSchema();
  return m('div', {class: 'pf-hde-card pf-hde-mb-4'}, [
    m('h3', {class: 'pf-hde-sub-heading'}, 'Bytes Retained by Heap'),
    m(DataGrid, {
      schema,
      rootSchema: 'query',
      data: dataRows,
      initialColumns: [
        {id: 'heap', field: 'heap'},
        {id: STATUS_COL, field: STATUS_COL},
        {
          id: deltaCol('total_size'),
          field: deltaCol('total_size'),
          sort: 'DESC',
        },
        {id: baselineCol('total_size'), field: baselineCol('total_size')},
        {id: currentCol('total_size'), field: currentCol('total_size')},
        {id: deltaCol('java_size'), field: deltaCol('java_size')},
        {id: baselineCol('java_size'), field: baselineCol('java_size')},
        {id: currentCol('java_size'), field: currentCol('java_size')},
        {id: deltaCol('native_size'), field: deltaCol('native_size')},
        {id: baselineCol('native_size'), field: baselineCol('native_size')},
        {id: currentCol('native_size'), field: currentCol('native_size')},
      ],
    }),
  ]);
}

function buildHeapsDiffSchema(): SchemaRegistry {
  const cols: Record<string, ColumnDef> = {
    heap: {title: 'Heap', columnType: 'text'},
    [STATUS_COL]: {
      title: 'Status',
      columnType: 'text',
      cellRenderer: statusRenderer,
    },
  };
  const fields: Array<{field: string; title: string}> = [
    {field: 'total_size', title: 'Total'},
    {field: 'java_size', title: 'Java'},
    {field: 'native_size', title: 'Native'},
  ];
  for (const f of fields) {
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
  return {query: cols};
}

function withTotalRow<T extends Row>(rows: T[], keyField: keyof T): Row[] {
  if (rows.length === 0) return rows;
  const total: Row = {[keyField as string]: 'Total'};
  for (const r of rows) {
    for (const k of Object.keys(r)) {
      if (k === keyField) continue;
      const v = r[k];
      if (typeof v === 'number') {
        total[k] = ((total[k] as number | undefined) ?? 0) + v;
      }
    }
  }
  return [total, ...rows];
}

// Roll-up row for diff-mode tables — sums each numeric field's
// baseline / current / delta across `rows`. Sets STATUS_COL based on the
// total `total_size` delta sign (GREW / SHRANK / UNCHANGED) — NEW /
// REMOVED don't apply to a synthetic aggregate.
function sumDiffRows(
  rows: ReadonlyArray<Row>,
  keyName: string,
  numericFields: ReadonlyArray<string>,
): Row {
  const out: Row = {[KEY_COL]: keyName};
  for (const field of numericFields) {
    let bSum = 0;
    let cSum = 0;
    for (const r of rows) {
      bSum += Number(r[baselineCol(field)] ?? 0);
      cSum += Number(r[currentCol(field)] ?? 0);
    }
    out[baselineCol(field)] = bSum;
    out[currentCol(field)] = cSum;
    out[deltaCol(field)] = cSum - bSum;
  }
  const totalDelta = Number(out[deltaCol('total_size')] ?? 0);
  out[STATUS_COL] =
    totalDelta > 0 ? 'GREW' : totalDelta < 0 ? 'SHRANK' : 'UNCHANGED';
  return out;
}

export default OverviewView;
