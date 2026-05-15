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
import {QuerySlot} from '../base/query_slot';
import {Icons} from '../base/semantic_icons';
import type {Trace} from '../public/trace';
import type {Dataset, DatasetSchema} from '../trace_processor/dataset';
import {NUM, type Row, type SqlValue} from '../trace_processor/query_result';
import {sqlValueToSqliteString} from '../trace_processor/sql_utils';
import {
  createPerfettoTable,
  type DisposableSqlEntity,
} from '../trace_processor/sql_utils';
import {Anchor} from '../widgets/anchor';
import {Button} from '../widgets/button';
import {DetailsShell} from '../widgets/details_shell';
import {Icon} from '../widgets/icon';
import {Section} from '../widgets/section';
import {Spinner} from '../widgets/spinner';
import {Tooltip} from '../widgets/tooltip';
import {Tree, TreeNode} from '../widgets/tree';
import {extensions} from './extensions';
import {DurationWidget} from './widgets/duration';
import {HistogramSvg} from './widgets/charts_svg/histogram_svg';
import {
  type HistogramData,
  SQLHistogramLoader,
} from './widgets/charts/histogram_loader';
import {DataGrid, renderCell} from './widgets/datagrid/datagrid';
import {SQLDataSource} from './widgets/datagrid/sql_data_source';
import type {
  ColumnSchema,
  SchemaRegistry,
} from './widgets/datagrid/datagrid_schema';
import type {Column, Filter} from './widgets/datagrid/model';
import type {
  SQLSchemaRegistry,
  SQLTableSchema,
} from './widgets/datagrid/sql_schema';
import {formatDuration} from './time_utils';

export function helpIcon(help: m.Children): m.Children {
  return m(
    Tooltip,
    {
      trigger: m(Icon, {
        className: 'pf-section-title-with-help__icon',
        icon: 'help_outline',
      }),
    },
    help,
  );
}

// Same layout as DistributionSummary but with placeholder values, so the
// surrounding panel doesn't reflow when the real summary loads.
export function renderDistributionPlaceholder(): m.Children {
  return m(
    '.pf-distribution-summary.pf-distribution-summary--placeholder',
    m('.pf-distribution-summary__placeholder-chart'),
    m(
      '.pf-distribution-summary__stats',
      m(
        Tree,
        {bordered: true},
        m(TreeNode, {left: 'Count', right: '-'}),
        m(TreeNode, {left: 'Total', right: '-'}),
        m(TreeNode, {left: 'Min', right: '-'}),
        m(TreeNode, {left: 'Mean', right: '-'}),
        m(TreeNode, {left: 'Max', right: '-'}),
      ),
      m(
        Tree,
        {bordered: true},
        m(TreeNode, {left: 'p50', right: '-'}),
        m(TreeNode, {left: 'p75', right: '-'}),
        m(TreeNode, {left: 'p95', right: '-'}),
        m(TreeNode, {left: 'p99', right: '-'}),
        m(TreeNode, {left: 'p99.9', right: '-'}),
      ),
    ),
  );
}

export function titleWithHelp(label: string, help: m.Children): m.Children {
  return m('span.pf-section-title-with-help', label, helpIcon(help));
}

export const SCOPE_HELP =
  'Which slices the histogram and instances list are computed over. ' +
  '"This track" stays inside the same kind of slices the selected one ' +
  'belongs to (e.g. main-thread slices only), useful when you care about ' +
  'a specific code path. "Across whole trace" pools matching slices from ' +
  'every track in the workspace, useful when the same name appears on ' +
  'many threads or processes and you want the full picture.';

export const HISTOGRAM_HELP =
  'Each bar groups matching slices by their duration: taller bars mean ' +
  'more slices ran for that long. Use this to spot the typical case ' +
  'versus outliers. A tight peak means consistent timing; a long right ' +
  'tail points to occasional slow runs that are usually worth ' +
  'investigating. The stats below summarize the same values; brushing a ' +
  'range on the chart focuses both the stats and the instances list on ' +
  'that subset.';

export const INSTANCES_HELP =
  'Every individual slice that matches the filter, one row per ' +
  'occurrence. Sorted by duration so the slowest sit at the top, which ' +
  'is usually where you want to start looking. Click an id to jump to ' +
  'that exact slice on the timeline and see the surrounding context ' +
  '(what came before, what thread it ran on, what it overlapped with).';

interface DistributionStats {
  readonly count: number;
  readonly min: number;
  readonly max: number;
  readonly mean: number;
  readonly sum: number;
  readonly p50: number;
  readonly p75: number;
  readonly p95: number;
  readonly p99: number;
  readonly p999: number;
}

// Computed once per source query so brushing doesn't shift the X-axis ticks.
interface NiceBuckets {
  readonly minValue: number;
  readonly bucketSize: number;
  readonly bucketCount: number;
}

const TARGET_BUCKETS = 50;

// Round v UP to the nearest 1/2/5 * 10^n, producing tick-aligned bucket sizes.
function niceCeil(v: number): number {
  if (v <= 0) return 1;
  const exp = Math.floor(Math.log10(v));
  const base = Math.pow(10, exp);
  const mantissa = v / base;
  if (mantissa <= 1) return base;
  if (mantissa <= 2) return 2 * base;
  if (mantissa <= 5) return 5 * base;
  return 10 * base;
}

function computeNiceBuckets(min: number, max: number): NiceBuckets {
  const range = Math.max(max - min, 1);
  const bucketSize = niceCeil(range / TARGET_BUCKETS);
  const minValue = Math.floor(min / bucketSize) * bucketSize;
  const upper = Math.ceil(max / bucketSize) * bucketSize;
  const bucketCount = Math.max(1, Math.round((upper - minValue) / bucketSize));
  return {minValue, bucketSize, bucketCount};
}

export interface DistributionInputs {
  readonly trace: Trace;
  readonly dataset: Dataset;

  // Optional extra equality filter applied on top of the dataset.
  // The dataset must include this column in its schema.
  readonly filter?: {readonly col: string; readonly eq: SqlValue};

  readonly valueColumn: string;
}

export interface DistributionSummaryAttrs extends DistributionInputs {
  // When provided, the summary reads from this pre-materialized table instead
  // of materializing one itself — used by DistributionPanel so the embedded
  // summary and the instances grid share a single materialized table.
  readonly sourceTable?: DisposableSqlEntity;

  readonly brush?: {readonly start: number; readonly end: number};
  readonly onBrushChange?: (
    brush: {readonly start: number; readonly end: number} | undefined,
  ) => void;

  // When set, the histogram bucket containing this value is drawn in a
  // distinct color — used to show "where does this slice's duration fall
  // in the distribution?".
  readonly highlightValue?: number;
}

// Reusable left-half: histogram (with brush) + percentile stats. Materializes
// the filtered dataset as a Perfetto table so the histogram and stats share
// a single aggregation source.
export class DistributionSummary
  implements m.ClassComponent<DistributionSummaryAttrs>
{
  private readonly tableSlot = new QuerySlot<DisposableSqlEntity>();
  private readonly boundsSlot = new QuerySlot<NiceBuckets>();
  private readonly statsSlot = new QuerySlot<DistributionStats>();

  private histogramLoader?: SQLHistogramLoader;
  private histogramLoaderTableName?: string;

  view({attrs}: m.CVnode<DistributionSummaryAttrs>): m.Children {
    const tableEntity = attrs.sourceTable ?? this.materializeSourceTable(attrs);
    if (tableEntity === undefined) {
      return m('.pf-distribution-summary__loading', m(Spinner, {easing: true}));
    }
    const tableName = tableEntity.name;
    const bounds = this.boundsSlot.use({
      key: {tableName, valueColumn: attrs.valueColumn},
      queryFn: () => fetchBounds(attrs, tableName),
    });
    const stats = this.statsSlot.use({
      key: {tableName, brush: attrs.brush},
      queryFn: () => fetchStats(attrs, tableName),
      retainOn: ['brush'],
    });
    const histogram = this.useHistogramLoader(attrs, tableName, bounds.data);

    return m(
      '.pf-distribution-summary',
      this.renderToolbar(attrs),
      this.renderHistogram(attrs, histogram.data),
      this.renderStats(attrs, stats.data, stats.isPending),
    );
  }

  onremove(): void {
    this.histogramLoader?.dispose();
    this.tableSlot.dispose();
    this.boundsSlot.dispose();
    this.statsSlot.dispose();
  }

  private materializeSourceTable(
    attrs: DistributionSummaryAttrs,
  ): DisposableSqlEntity | undefined {
    const sourceQuery = buildSourceQuery(attrs, [attrs.valueColumn]);
    return this.tableSlot.use({
      key: {sourceQuery},
      queryFn: () =>
        createPerfettoTable({engine: attrs.trace.engine, as: sourceQuery}),
    }).data;
  }

  private useHistogramLoader(
    attrs: DistributionSummaryAttrs,
    tableName: string,
    bounds: NiceBuckets | undefined,
  ) {
    let loader = this.histogramLoader;
    if (loader === undefined || this.histogramLoaderTableName !== tableName) {
      loader?.dispose();
      loader = new SQLHistogramLoader({
        engine: attrs.trace.engine,
        query: `SELECT ${attrs.valueColumn} FROM ${tableName}`,
        valueColumn: attrs.valueColumn,
      });
      this.histogramLoader = loader;
      this.histogramLoaderTableName = tableName;
    }
    return loader.use({
      bucketCount: bounds?.bucketCount ?? TARGET_BUCKETS,
      bucketSize: bounds?.bucketSize,
      minValue: bounds?.minValue,
      filter:
        attrs.brush !== undefined
          ? {min: attrs.brush.start, max: attrs.brush.end}
          : undefined,
    });
  }

  private renderToolbar(attrs: DistributionSummaryAttrs): m.Children {
    const canReset =
      attrs.brush !== undefined && attrs.onBrushChange !== undefined;
    return m(
      '.pf-distribution-summary__histogram-toolbar',
      canReset &&
        m(Button, {
          label: 'Reset selection',
          icon: Icons.Close,
          compact: true,
          onclick: () => attrs.onBrushChange?.(undefined),
        }),
    );
  }

  private renderHistogram(
    attrs: DistributionSummaryAttrs,
    data: HistogramData | undefined,
  ): m.Children {
    const onBrushChange = attrs.onBrushChange;
    return m(HistogramSvg, {
      data,
      height: 220,
      xAxisLabel: attrs.valueColumn,
      yAxisLabel: 'Count',
      formatXValue: (v) => formatDuration(attrs.trace, BigInt(Math.round(v))),
      onBrush:
        onBrushChange === undefined ? undefined : (r) => onBrushChange(r),
      selection:
        attrs.brush ??
        (attrs.highlightValue !== undefined
          ? {start: attrs.highlightValue, end: attrs.highlightValue}
          : undefined),
    });
  }

  private renderStats(
    attrs: DistributionSummaryAttrs,
    stats: DistributionStats | undefined,
    isPending: boolean,
  ): m.Children {
    if (stats === undefined) {
      return m(
        '.pf-distribution-summary__stats',
        isPending ? m(Spinner) : 'No data',
      );
    }
    const dur = (v: number) =>
      m(DurationWidget, {trace: attrs.trace, dur: BigInt(Math.round(v))});
    return m(
      '.pf-distribution-summary__stats',
      m(
        Tree,
        {bordered: true},
        m(TreeNode, {left: 'Count', right: stats.count.toLocaleString()}),
        m(TreeNode, {left: 'Total', right: dur(stats.sum)}),
        m(TreeNode, {left: 'Min', right: dur(stats.min)}),
        m(TreeNode, {left: 'Mean', right: dur(stats.mean)}),
        m(TreeNode, {left: 'Max', right: dur(stats.max)}),
      ),
      m(
        Tree,
        {bordered: true},
        m(TreeNode, {left: 'p50', right: dur(stats.p50)}),
        m(TreeNode, {left: 'p75', right: dur(stats.p75)}),
        m(TreeNode, {left: 'p95', right: dur(stats.p95)}),
        m(TreeNode, {left: 'p99', right: dur(stats.p99)}),
        m(TreeNode, {left: 'p99.9', right: dur(stats.p999)}),
      ),
    );
  }
}

export interface DistributionPanelAttrs extends DistributionInputs {
  // Id column used to navigate to a specific instance.
  readonly idColumn: string;

  // SQL table name passed to selectSqlEvent on id-cell click.
  readonly sqlTable: string;

  // Columns to display in the grid, in order. Must all be in dataset.schema.
  readonly displayColumns: ReadonlyArray<string>;

  // Per-column overrides; defaults to the DataGrid's plain-value renderer.
  readonly cellRenderers?: Readonly<
    Record<string, (value: Row[string]) => m.Children>
  >;

  readonly title?: string;
}

// Two-pane "value distribution" tab: instances grid + histogram summary,
// both reading from a single materialized Perfetto table.
export class DistributionPanel
  implements m.ClassComponent<DistributionPanelAttrs>
{
  private readonly tableSlot = new QuerySlot<DisposableSqlEntity>();

  private dataSource?: SQLDataSource;
  private dataSourceTableName?: string;
  private brush?: {start: number; end: number};

  view({attrs}: m.CVnode<DistributionPanelAttrs>): m.Children {
    const tableEntity = this.materializeSourceTable(attrs);
    return m(
      DetailsShell,
      {
        title: panelTitle(attrs),
        description: attrs.sqlTable,
        fillHeight: true,
        buttons: this.renderAddDebugTrackButton(attrs),
      },
      m(
        '.pf-distribution-panel',
        this.renderInstancesPane(attrs, tableEntity),
        this.renderHistogramPane(attrs, tableEntity),
      ),
    );
  }

  private renderAddDebugTrackButton(attrs: DistributionPanelAttrs): m.Children {
    return m(Button, {
      label: 'Add debug track',
      onclick: () => {
        const baseQuery = buildSourceQuery(attrs, [
          attrs.idColumn,
          attrs.valueColumn,
          ...attrs.displayColumns,
        ]);
        const brush = this.brush;
        const sqlSource =
          brush === undefined
            ? baseQuery
            : `SELECT * FROM (${baseQuery}) WHERE ${attrs.valueColumn} ` +
              `BETWEEN ${brush.start} AND ${brush.end}`;
        extensions.addDebugSliceTrack({
          trace: attrs.trace,
          data: {sqlSource},
          title: panelTitle(attrs),
        });
      },
    });
  }

  onremove(): void {
    this.dataSource?.dispose();
    this.tableSlot.dispose();
  }

  private materializeSourceTable(
    attrs: DistributionPanelAttrs,
  ): DisposableSqlEntity | undefined {
    const sourceQuery = buildSourceQuery(attrs, [
      attrs.idColumn,
      attrs.valueColumn,
      ...attrs.displayColumns,
    ]);
    return this.tableSlot.use({
      key: {sourceQuery},
      queryFn: () =>
        createPerfettoTable({engine: attrs.trace.engine, as: sourceQuery}),
    }).data;
  }

  private renderInstancesPane(
    attrs: DistributionPanelAttrs,
    tableEntity: DisposableSqlEntity | undefined,
  ): m.Children {
    return m(
      '.pf-distribution-panel__instances',
      m(
        Section,
        {title: titleWithHelp('Instances', INSTANCES_HELP)},
        tableEntity === undefined
          ? m(Spinner, {easing: true})
          : this.renderGrid(attrs, tableEntity),
      ),
    );
  }

  private renderHistogramPane(
    attrs: DistributionPanelAttrs,
    tableEntity: DisposableSqlEntity | undefined,
  ): m.Children {
    return m(
      '.pf-distribution-panel__histogram',
      m(
        Section,
        {title: titleWithHelp('Histogram', HISTOGRAM_HELP)},
        tableEntity === undefined
          ? m(Spinner, {easing: true})
          : m(DistributionSummary, {
              trace: attrs.trace,
              dataset: attrs.dataset,
              filter: attrs.filter,
              valueColumn: attrs.valueColumn,
              sourceTable: tableEntity,
              brush: this.brush,
              onBrushChange: (b) => {
                this.brush = b;
              },
            }),
      ),
    );
  }

  private renderGrid(
    attrs: DistributionPanelAttrs,
    tableEntity: DisposableSqlEntity,
  ): m.Children {
    const dataSource = this.useDataSource(attrs, tableEntity.name);
    return m(DataGrid, {
      schema: buildGridSchema(attrs, this.renderIdCell.bind(this)),
      rootSchema: 'root',
      data: dataSource,
      filters: brushFilters(attrs.valueColumn, this.brush),
      initialColumns: gridColumns(attrs),
      fillHeight: true,
      emptyStateMessage: 'No matching instances',
    });
  }

  private useDataSource(
    attrs: DistributionPanelAttrs,
    tableName: string,
  ): SQLDataSource {
    let ds = this.dataSource;
    if (ds === undefined || this.dataSourceTableName !== tableName) {
      ds?.dispose();
      ds = new SQLDataSource({
        engine: attrs.trace.engine,
        sqlSchema: buildSqlSchema(attrs, tableName),
        rootSchemaName: 'root',
      });
      this.dataSource = ds;
      this.dataSourceTableName = tableName;
      this.brush = undefined;
    }
    return ds;
  }

  private renderIdCell(
    attrs: DistributionPanelAttrs,
    value: Row[string],
  ): m.Children {
    const cell = renderCell(value, attrs.idColumn);
    const id = toNumericId(value);
    if (id === undefined) return cell;
    return m(
      Anchor,
      {
        title: `Go to ${attrs.sqlTable} on the timeline`,
        icon: Icons.UpdateSelection,
        onclick: () =>
          attrs.trace.selection.selectSqlEvent(attrs.sqlTable, id, {
            scrollToSelection: true,
          }),
      },
      cell,
    );
  }
}

function panelTitle(attrs: DistributionPanelAttrs): string {
  if (attrs.title !== undefined) return attrs.title;
  if (attrs.filter !== undefined) return String(attrs.filter.eq);
  return attrs.valueColumn;
}

function buildGridSchema(
  attrs: DistributionPanelAttrs,
  renderIdCell: (
    attrs: DistributionPanelAttrs,
    value: Row[string],
  ) => m.Children,
): SchemaRegistry {
  const rootSchema: ColumnSchema = {};
  for (const col of [attrs.idColumn, ...attrs.displayColumns]) {
    const cellRenderer =
      col === attrs.idColumn
        ? (value: Row[string]) => renderIdCell(attrs, value)
        : attrs.cellRenderers?.[col];
    rootSchema[col] = {title: col, cellRenderer};
  }
  return {root: rootSchema};
}

function gridColumns(attrs: DistributionPanelAttrs): Column[] {
  return [
    {id: attrs.idColumn, field: attrs.idColumn},
    ...attrs.displayColumns.map(
      (field): Column => ({
        id: field,
        field,
        sort: field === attrs.valueColumn ? 'DESC' : undefined,
      }),
    ),
  ];
}

function brushFilters(
  valueColumn: string,
  brush: {start: number; end: number} | undefined,
): Filter[] {
  if (brush === undefined) return [];
  return [
    {field: valueColumn, op: '>=', value: brush.start},
    {field: valueColumn, op: '<=', value: brush.end},
  ];
}

function toNumericId(value: Row[string]): number | undefined {
  if (typeof value === 'bigint') return Number(value);
  if (typeof value === 'number') return value;
  return undefined;
}

function buildSourceQuery(
  inputs: {
    readonly dataset: Dataset;
    readonly filter?: {readonly col: string; readonly eq: SqlValue};
  },
  columns: ReadonlyArray<string>,
): string {
  const baseQuery = inputs.dataset.query(requiredSchema(inputs, columns));
  if (inputs.filter === undefined) {
    return baseQuery;
  }
  const literal = sqlValueToSqliteString(inputs.filter.eq);
  return `SELECT * FROM (${baseQuery}) WHERE ${inputs.filter.col} = ${literal}`;
}

function requiredSchema(
  inputs: {
    readonly dataset: Dataset;
    readonly filter?: {readonly col: string; readonly eq: SqlValue};
  },
  columns: ReadonlyArray<string>,
): DatasetSchema {
  const schema: Record<string, SqlValue> = {};
  for (const col of new Set([
    ...columns,
    ...(inputs.filter === undefined ? [] : [inputs.filter.col]),
  ])) {
    schema[col] = inputs.dataset.schema[col];
  }
  return schema;
}

function buildSqlSchema(
  attrs: DistributionPanelAttrs,
  tableName: string,
): SQLSchemaRegistry {
  const columns: SQLTableSchema['columns'] = {};
  columns[attrs.idColumn] = {};
  columns[attrs.valueColumn] = {};
  for (const col of attrs.displayColumns) {
    columns[col] = {};
  }
  return {
    root: {
      table: tableName,
      primaryKey: attrs.idColumn,
      columns,
    },
  };
}

async function fetchBounds(
  inputs: DistributionInputs,
  tableName: string,
): Promise<NiceBuckets> {
  const v = inputs.valueColumn;
  const result = await inputs.trace.engine.query(`
    SELECT IFNULL(MIN(${v}), 0) AS minv, IFNULL(MAX(${v}), 0) AS maxv
    FROM ${tableName}
  `);
  const it = result.iter({minv: NUM, maxv: NUM});
  if (!it.valid()) {
    return {minValue: 0, bucketSize: 1, bucketCount: 1};
  }
  return computeNiceBuckets(it.minv, it.maxv);
}

async function fetchStats(
  inputs: DistributionInputs,
  tableName: string,
): Promise<DistributionStats> {
  const v = inputs.valueColumn;
  const result = await inputs.trace.engine.query(`
    SELECT
      COUNT(*) AS cnt,
      IFNULL(MIN(${v}), 0) AS minv,
      IFNULL(MAX(${v}), 0) AS maxv,
      IFNULL(AVG(${v}), 0) AS meanv,
      IFNULL(SUM(${v}), 0) AS sumv,
      IFNULL(PERCENTILE(${v}, 50), 0) AS p50,
      IFNULL(PERCENTILE(${v}, 75), 0) AS p75,
      IFNULL(PERCENTILE(${v}, 95), 0) AS p95,
      IFNULL(PERCENTILE(${v}, 99), 0) AS p99,
      IFNULL(PERCENTILE(${v}, 99.9), 0) AS p999
    FROM ${tableName}
  `);
  const it = result.iter({
    cnt: NUM,
    minv: NUM,
    maxv: NUM,
    meanv: NUM,
    sumv: NUM,
    p50: NUM,
    p75: NUM,
    p95: NUM,
    p99: NUM,
    p999: NUM,
  });
  if (!it.valid()) {
    return {
      count: 0,
      min: 0,
      max: 0,
      mean: 0,
      sum: 0,
      p50: 0,
      p75: 0,
      p95: 0,
      p99: 0,
      p999: 0,
    };
  }
  return {
    count: it.cnt,
    min: it.minv,
    max: it.maxv,
    mean: it.meanv,
    sum: it.sumv,
    p50: it.p50,
    p75: it.p75,
    p95: it.p95,
    p99: it.p99,
    p999: it.p999,
  };
}

// Opens DistributionPanel in an ephemeral tab keyed by sqlTable/value/filter
// so re-invoking with the same arguments reuses the existing tab.
export function openDistributionTab(
  trace: Trace,
  config: Omit<DistributionPanelAttrs, 'trace'>,
): void {
  const filterKey =
    config.filter !== undefined
      ? `${config.filter.col}=${sqlValueToSqliteString(config.filter.eq)}`
      : '*';
  const title = panelTitle({trace, ...config});
  // Title is part of the URI so two tabs of distinct scope (e.g. across-track
  // vs in-selection) can coexist for the same (table, column, filter).
  const uri = `distribution#${config.sqlTable}/${config.valueColumn}/${filterKey}/${title}`;

  trace.tabs.registerTab({
    uri,
    isEphemeral: true,
    content: {
      getTitle: () => title,
      render: () => m(DistributionPanel, {trace, ...config}),
    },
  });
  trace.tabs.showTab(uri);
}
