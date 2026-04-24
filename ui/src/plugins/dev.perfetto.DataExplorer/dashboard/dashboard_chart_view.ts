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
import {Trace} from '../../../public/trace';
import {
  ChartConfig,
  ChartType,
  generateChartId,
} from '../query_builder/nodes/visualisation_node';
import {
  ChartColumnProvider,
  ChartLoaderEntry,
  buildLoaderCacheKey,
  createChartLoaders,
  disposeChartLoaders,
  renderChartByType,
} from '../query_builder/charts/chart_renderers';
import {ColumnInfo} from '../query_builder/column_info';
import {
  DashboardBrushFilter,
  DashboardDataSource,
  DashboardItem,
  getItemId,
  getLinkedSourceNodeIds,
} from './dashboard_registry';
import {ResultsPanelEmptyState} from '../query_builder/widgets';
import {SqlValue} from '../../../trace_processor/query_result';
import {isQuantitativeType} from '../../../trace_processor/perfetto_sql_type';

export interface DashboardChartViewAttrs {
  trace: Trace;
  source: DashboardDataSource;
  config: ChartConfig;
  dashboardId: string;
  items: DashboardItem[];
  allSources: ReadonlyArray<DashboardDataSource>;
  brushFilters: Map<string, DashboardBrushFilter[]>;
  onItemsChange: (items: DashboardItem[]) => void;
  onBrushFiltersChange: (filters: Map<string, DashboardBrushFilter[]>) => void;
  /**
   * When true, this chart is a "driver" (above a segment divider).
   * Driver charts show brush selection overlays but do NOT apply brush
   * filters to their own SQL queries — they only propagate filters to
   * consumer charts below dividers.
   */
  isDriverChart?: boolean;
  /** Grid lines setting passed through to chart widgets. */
  gridLines?: 'horizontal' | 'vertical' | 'both';
}

/** Subset of DashboardChartViewAttrs needed by the adapter. */
export interface DashboardChartCallbacks {
  dashboardId: string;
  items: DashboardItem[];
  allSources: ReadonlyArray<DashboardDataSource>;
  brushFilters: Map<string, DashboardBrushFilter[]>;
  onItemsChange: (items: DashboardItem[]) => void;
  onBrushFiltersChange: (filters: Map<string, DashboardBrushFilter[]>) => void;
}

/**
 * Adapter implementing ChartColumnProvider for a DashboardDataSource.
 * Config and brush-filter changes are propagated via callbacks on the
 * owning dashboard tab (no global registry dependency).
 */
class DashboardChartAdapter implements ChartColumnProvider {
  private readonly cols: ColumnInfo[];
  private readonly sourceNodeId: string;
  // Local mutable copy of brush filters for this source, so that
  // clearChartFiltersForColumn + setBrushSelection works the same way as
  // the visualisation node (synchronous mutations on the same array).
  private filters: DashboardBrushFilter[];
  // Columns explicitly cleared since the last flush — used by flushFilters
  // to propagate clears to other datasources even when no new filters are
  // added for the cleared column.
  private pendingClearedColumns = new Set<string>();
  private callbacks: DashboardChartCallbacks;
  private config: ChartConfig;

  constructor(
    source: DashboardDataSource,
    callbacks: DashboardChartCallbacks,
    config: ChartConfig,
  ) {
    this.sourceNodeId = source.nodeId;
    this.callbacks = callbacks;
    this.config = config;
    this.filters = [...(callbacks.brushFilters.get(source.nodeId) ?? [])];
    this.cols = source.columns.map((col) => ({
      name: col.name,
      checked: false,
      type: col.type,
    }));
  }

  /** Update mutable references so the adapter flushes to the latest attrs. */
  updateCallbacks(callbacks: DashboardChartCallbacks, config: ChartConfig) {
    this.callbacks = callbacks;
    this.config = config;
    this.filters = [...(callbacks.brushFilters.get(this.sourceNodeId) ?? [])];
  }

  get sourceCols(): ReadonlyArray<ColumnInfo> {
    return this.cols;
  }

  getChartableColumns(chartType: ChartType): ReadonlyArray<ColumnInfo> {
    if (
      chartType === 'histogram' ||
      chartType === 'line' ||
      chartType === 'scatter' ||
      chartType === 'cdf'
    ) {
      return this.cols.filter(
        (col) => col.type !== undefined && isQuantitativeType(col.type),
      );
    }
    return this.cols;
  }

  private flushFilters(): void {
    const newMap = new Map(this.callbacks.brushFilters);
    if (this.filters.length === 0) {
      newMap.delete(this.sourceNodeId);
    } else {
      newMap.set(this.sourceNodeId, [...this.filters]);
    }

    // Cross-datasource brushing: propagate filters to other sources that
    // share columns with the same name. Also clear columns that were
    // explicitly cleared (pendingClearedColumns) even if no new filters
    // were added for them.
    const touchedColumns = new Set([
      ...this.filters.map((f) => f.column),
      ...this.pendingClearedColumns,
    ]);
    this.pendingClearedColumns.clear();

    for (const col of touchedColumns) {
      const newFiltersForCol = this.filters.filter((f) => f.column === col);
      for (const id of getLinkedSourceNodeIds(
        this.callbacks.allSources,
        this.sourceNodeId,
        col,
      )) {
        if (id === this.sourceNodeId) continue;
        const existing = newMap.get(id) ?? [];
        const retained = existing.filter((f) => f.column !== col);
        const combined = [...retained, ...newFiltersForCol];
        if (combined.length === 0) {
          newMap.delete(id);
        } else {
          newMap.set(id, combined);
        }
      }
    }

    this.callbacks.onBrushFiltersChange(newMap);
  }

  clearChartFiltersForColumn(column: string): void {
    this.clearColumnLocally(column);
    this.flushFilters();
  }

  setBrushSelection(column: string, values: SqlValue[]): void {
    this.clearColumnLocally(column);
    for (const value of values) {
      if (value === null) {
        this.filters.push({column, op: 'is null'});
      } else {
        this.filters.push({column, op: '=', value});
      }
    }
    this.flushFilters();
    m.redraw();
  }

  addRangeFilter(column: string, min: number, max: number): void {
    this.clearColumnLocally(column);
    this.filters.push({column, op: '>=', value: min});
    this.filters.push({column, op: '<', value: max});
    this.flushFilters();
    m.redraw();
  }

  /** Remove filters for `column` locally without flushing. */
  private clearColumnLocally(column: string): void {
    this.filters = this.filters.filter((f) => f.column !== column);
    this.pendingClearedColumns.add(column);
  }

  updateChart(
    chartId: string,
    updates: Partial<Omit<ChartConfig, 'id'>>,
  ): void {
    const items = this.callbacks.items.map((i) => {
      if (i.kind === 'chart' && i.config.id === chartId) {
        return {...i, config: {...i.config, ...updates}};
      }
      return i;
    });
    this.callbacks.onItemsChange(items);
  }

  removeChart(chartId: string): void {
    const items = this.callbacks.items.filter((i) => getItemId(i) !== chartId);
    this.callbacks.onItemsChange(items);
  }

  get attrs() {
    return {chartConfigs: [this.config]};
  }
}

/**
 * Renders a single chart for a dashboard data source.
 * Only renders chart content — the card wrapper, header, resize handles,
 * and drag-and-drop are handled by the Dashboard component.
 *
 * Table name resolution is handled eagerly by DashboardNode — the source's
 * `tableName` field is populated when the upstream node has been executed.
 * If the table name is not yet available, this component triggers execution
 * via `source.requestExecution()` and waits for a redraw.
 */
export class DashboardChartView
  implements m.ClassComponent<DashboardChartViewAttrs>
{
  /** Adapter class exposed for use by Dashboard's config popup. */
  static Adapter = DashboardChartAdapter;

  private loaders = new Map<string, ChartLoaderEntry>();
  private executionRequested = false;
  private cachedAdapter?: DashboardChartAdapter;
  private adapterSourceNodeId?: string;
  private adapterConfigId?: string;

  onremove() {
    for (const entry of this.loaders.values()) {
      disposeChartLoaders(entry);
    }
    this.loaders.clear();
  }

  private getAdapter(attrs: DashboardChartViewAttrs): DashboardChartAdapter {
    // Re-create the adapter only when the source or config identity changes.
    if (
      this.cachedAdapter === undefined ||
      this.adapterSourceNodeId !== attrs.source.nodeId ||
      this.adapterConfigId !== attrs.config.id
    ) {
      this.cachedAdapter = new DashboardChartAdapter(
        attrs.source,
        attrs,
        attrs.config,
      );
      this.adapterSourceNodeId = attrs.source.nodeId;
      this.adapterConfigId = attrs.config.id;
    } else {
      // Update the callbacks reference so the adapter always flushes to
      // the latest attrs (items, brushFilters, etc.).
      this.cachedAdapter.updateCallbacks(attrs, attrs.config);
    }
    return this.cachedAdapter;
  }

  view({attrs}: m.CVnode<DashboardChartViewAttrs>) {
    const adapter = this.getAdapter(attrs);
    const config = attrs.config;

    if (config.column === '') {
      return m(ResultsPanelEmptyState, {
        icon: 'ssid_chart',
        title: 'Select a column',
      });
    }

    // Validate that the configured column exists in the current source.
    const columnExists = attrs.source.columns.some(
      (c) => c.name === config.column,
    );
    if (!columnExists) {
      return m(
        ResultsPanelEmptyState,
        {icon: 'warning', title: 'Invalid column'},
        `Column "${config.column}" not found in this data source.`,
      );
    }

    // If the table name isn't available yet, trigger execution once and wait.
    if (attrs.source.tableName === undefined) {
      if (!this.executionRequested && attrs.source.requestExecution) {
        this.executionRequested = true;
        attrs.source
          .requestExecution()
          .catch((e) => console.debug('Dashboard source execution failed:', e))
          .finally(() => {
            this.executionRequested = false;
          });
      }
      return m(ResultsPanelEmptyState, {
        icon: 'hourglass_empty',
        title: 'Loading data…',
      });
    }

    const entry = this.ensureLoader(attrs, config);
    const ctx = {
      node: adapter,
      onFilterChange: () => m.redraw(),
      gridLines: attrs.gridLines,
    };
    return renderChartByType(ctx, config, entry);
  }

  private ensureLoader(
    attrs: DashboardChartViewAttrs,
    config: ChartConfig,
  ): ChartLoaderEntry {
    const tableName = attrs.source.tableName;
    const columnValid = attrs.source.columns.some(
      (c) => c.name === config.column,
    );
    if (tableName === undefined || config.column === '' || !columnValid) {
      let entry = this.loaders.get(config.id);
      if (entry === undefined) {
        entry = {key: ''};
        this.loaders.set(config.id, entry);
      }
      return entry;
    }

    // Driver charts (above a divider) show brush overlays but don't filter
    // their own data — skip the WHERE clause entirely.
    // Drop brush filters referencing columns that don't exist in the current
    // source (can happen after switching the chart's data source).
    const validColumns = new Set(attrs.source.columns.map((c) => c.name));
    const filters = attrs.isDriverChart
      ? []
      : (attrs.brushFilters.get(attrs.source.nodeId) ?? []).filter((f) =>
          validColumns.has(f.column),
        );
    const filterKey = JSON.stringify(filters);

    const key = buildLoaderCacheKey(tableName, config, filterKey);

    const existing = this.loaders.get(config.id);
    if (existing !== undefined && existing.key === key) return existing;

    if (existing !== undefined) {
      disposeChartLoaders(existing);
    }

    const entry: ChartLoaderEntry = {key};
    this.loaders.set(config.id, entry);

    const engine = attrs.trace.engine;
    const whereClause = buildWhereClause(filters);
    const query = `SELECT * FROM ${tableName}${whereClause}`;
    createChartLoaders(engine, query, config, entry);

    return entry;
  }
}

/**
 * Build a SQL WHERE clause from dashboard brush filters.
 * Same-column '=' filters are combined with OR (IN clause).
 * Range filters and cross-column conditions are combined with AND.
 */
function buildWhereClause(
  filters: ReadonlyArray<DashboardBrushFilter>,
): string {
  if (filters.length === 0) return '';

  // Group filters by column.
  const byColumn = new Map<string, DashboardBrushFilter[]>();
  for (const f of filters) {
    const list = byColumn.get(f.column) ?? [];
    list.push(f);
    byColumn.set(f.column, list);
  }

  const clauses: string[] = [];
  for (const [column, colFilters] of byColumn) {
    const eqValues: SqlValue[] = [];
    let hasNull = false;
    const rangeConditions: string[] = [];

    for (const f of colFilters) {
      if (f.op === '=') {
        eqValues.push(f.value ?? null);
      } else if (f.op === 'is null') {
        hasNull = true;
      } else {
        // >= or <
        rangeConditions.push(`${column} ${f.op} ${f.value}`);
      }
    }

    if (eqValues.length > 0 || hasNull) {
      const parts: string[] = [];
      if (eqValues.length === 1) {
        const v = eqValues[0];
        parts.push(
          `${column} = ${typeof v === 'string' ? `'${v.replace(/'/g, "''")}'` : v}`,
        );
      } else if (eqValues.length > 1) {
        const formatted = eqValues.map((v) =>
          typeof v === 'string' ? `'${v.replace(/'/g, "''")}'` : v,
        );
        parts.push(`${column} IN (${formatted.join(', ')})`);
      }
      if (hasNull) {
        parts.push(`${column} IS NULL`);
      }
      clauses.push(parts.length > 1 ? `(${parts.join(' OR ')})` : parts[0]);
    }

    for (const rc of rangeConditions) {
      clauses.push(rc);
    }
  }

  return ` WHERE ${clauses.join(' AND ')}`;
}

/**
 * Create a default ChartConfig for a data source, picking the first column.
 */
export function createDefaultChartConfig(
  columns: ReadonlyArray<{name: string}>,
  chartType: ChartType = 'bar',
): ChartConfig {
  const column = columns.length > 0 ? columns[0].name : '';
  return {
    id: generateChartId(),
    column,
    chartType,
  };
}
