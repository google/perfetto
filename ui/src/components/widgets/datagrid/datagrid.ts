// Copyright (C) 2025 The Android Open Source Project
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
import {intersperse} from '../../../base/array_utils';
import {classNames} from '../../../base/classnames';
import {download} from '../../../base/download_utils';
import {Icons} from '../../../base/semantic_icons';
import {exists, isNumeric, maybeUndefined} from '../../../base/utils';
import {Row, SqlValue} from '../../../trace_processor/query_result';
import {Anchor} from '../../../widgets/anchor';
import {Button, ButtonVariant} from '../../../widgets/button';
import {EmptyState} from '../../../widgets/empty_state';
import {
  Grid,
  GridApi,
  GridCell,
  GridColumn,
  GridHeaderCell,
  renderSortMenuItems,
} from '../../../widgets/grid';
import {Icon} from '../../../widgets/icon';
import {LinearProgress} from '../../../widgets/linear_progress';
import {MenuDivider, MenuItem} from '../../../widgets/menu';
import {
  AggregateMenu,
  ColumnMenu,
  getAggregateFunctionsForColumnType,
} from './add_column_menu';
import {CellFilterMenu} from './cell_filter_menu';
import {FilterMenu} from './column_filter_menu';
import {ColumnInfoMenu} from './column_info_menu';
import {DataSource} from './data_source';
import {
  SchemaRegistry,
  getColumnInfo,
  getDefaultVisibleColumns,
  isCellRenderResult,
} from './datagrid_schema';
import {DataGridToolbar, GridFilterChip} from './datagrid_toolbar';
import {DataGridExportButton} from './export_button';
import {
  buildColumnNames,
  defaultValueFormatter,
  formatAsJSON,
  formatAsMarkdown,
  formatAsTSV,
  formatRows,
} from './export_utils';
import {InMemoryDataSource} from './in_memory_data_source';
import {
  AggregateColumn,
  AggregateFunction,
  Column,
  Filter,
  Pivot,
  SortDirection,
} from './model';

export interface AggregationCellAttrs extends m.Attributes {
  readonly symbol?: string;
  readonly isLoading?: boolean;
}

export class AggregationCell implements m.ClassComponent<AggregationCellAttrs> {
  view({attrs, children}: m.Vnode<AggregationCellAttrs>) {
    const {className, symbol, isLoading, ...rest} = attrs;
    return m(
      '.pf-aggr-cell',
      {
        ...rest,
        className: classNames(className),
      },
      m('.pf-aggr-cell__symbol', symbol),
      m(
        '.pf-aggr-cell__content',
        isLoading ? m('.pf-aggr-cell__loading-spinner') : children,
      ),
    );
  }
}

/**
 * DataGrid is designed to be a flexible and efficient data viewing and analysis
 * tool. DataGrid at its core merely defines how the UI looks and UX feels. It
 * has no opinions about how data should be loaded or cached - that's what
 * DataSources are for.
 *
 * DataGrid comes with a few datasources out of the box - such as a simple
 * in-memory one where the source data is a simply javascript array and all the
 * operations are performed in javascript, and a simple SQL one where the source
 * of truth is defined using a SQL query and the data is loaded from TP.
 *
 * Most of DataGrid's state can operate in controlled or uncontrolled modes,
 * which allow the developer using this component to store the state anywhere
 * they please and also control it and mutate it from outside (controlled mode),
 * or it can operate internally which can make it easier to get up and running
 * (uncontrolled mode).
 */

export interface DataGridAttrs {
  /**
   * Fill parent container vertically.
   */
  readonly fillHeight?: boolean;

  /**
   * Optional class name added to the root element of the data grid.
   */
  readonly className?: string;

  /**
   * Schema registry defining the shape of available data.
   * Contains named schemas that can reference each other for nested relationships.
   */
  readonly schema: SchemaRegistry;

  /**
   * The name of the root schema in the registry to use for this grid.
   */
  readonly rootSchema: string;

  /**
   * The data source that provides rows to the grid. Responsible for fetching,
   * filtering, and sorting data based on the current state.
   *
   * The data source is responsible for applying the filters, sorting, and
   * paging and providing the rows that are displayed in the grid.
   */
  readonly data: DataSource | readonly Row[];

  /**
   * Array of column paths that are currently visible, in display order.
   * Each path is a dot-separated string like 'id', 'parent.name', 'thread.process.pid'.
   *
   * In controlled mode: Provide this prop along with onColumnsChanged callback.
   * In uncontrolled mode: Omit this prop to let the grid manage columns internally,
   * defaulting to all leaf columns in the root schema.
   *
   * When pivot mode is active (pivot prop is set without drillDown), the pivot
   * groupBy and aggregate columns take precedence for display. However, columns
   * is still used when drilling down into pivot groups.
   *
   * Each column can be either a string (column path) or a DataGridColumn object
   * with an optional aggregation function.
   */
  readonly columns?: readonly Column[];

  /**
   * Initial columns to show on first load.
   * This is ignored in controlled mode (i.e. when `columns` is provided).
   */
  readonly initialColumns?: readonly Column[];

  /**
   * Callback triggered when visible columns change (add, remove, reorder).
   * Required for controlled mode - when provided with columns,
   * the parent component becomes responsible for updating the columns prop.
   */
  readonly onColumnsChanged?: (columns: readonly Column[]) => void;

  /**
   * Array of filters to apply to the data - can operate in controlled or
   * uncontrolled mode.
   *
   * In controlled mode: Provide this prop along with onFiltersChanged callback.
   * In uncontrolled mode: Omit this prop to let the grid manage filter state
   * internally.
   *
   * Each filter contains a column name, operator, and comparison value. If not
   * provided, defaults to an empty array (no filters initially applied).
   */
  readonly filters?: readonly Filter[];

  /**
   * Initial filters to apply to the grid on first load.
   * This is ignored in controlled mode (i.e. when `filters` is provided).
   */
  readonly initialFilters?: readonly Filter[];

  // Called when a filter is added.
  readonly onFilterAdd?: (filter: Filter) => void;

  // Called after onFilterAdd when any filters are changed.
  readonly onFiltersChanged?: (filters: readonly Filter[]) => void;

  /**
   * Defines how data should be pivoted - can operate in controlled or
   * uncontrolled mode.
   *
   * In controlled mode: Provide this prop along with onPivotChanged callback.
   * In uncontrolled mode: Omit this prop to let the grid manage pivot state
   * internally.
   *
   * Specifies groupBy columns and aggregation values for pivoting.
   * If not provided, defaults to undefined (no pivoting).
   */
  readonly pivot?: Pivot;

  /**
   * Initial pivot configuration to apply on first load.
   * This is ignored in controlled mode (i.e. when `pivot` is provided).
   */
  readonly initialPivot?: Pivot;

  /**
   * Callback triggered when the pivot configuration changes.
   * Allows parent components to react to pivot changes.
   * Required for controlled mode - when provided with pivot,
   * the parent component becomes responsible for updating the pivot prop.
   */
  readonly onPivotChanged?: (pivot: Pivot | undefined) => void;

  /**
   * Extra items to place on the toolbar.
   */
  readonly toolbarItemsLeft?: m.Children;

  /**
   * Extra items to place on the toolbar.
   */
  readonly toolbarItemsRight?: m.Children;

  /**
   * When true, shows the export button on the toolbar.
   * Default = false.
   */
  readonly showExportButton?: boolean;

  /**
   * When true, enables pivot controls that allow users to modify the pivot
   * structure (add/remove groupBy columns, add/remove aggregates, change
   * aggregate functions, drill down). When false, pivot controls are hidden
   * and the pivot structure becomes read-only.
   *
   * Default = true.
   */
  readonly enablePivotControls?: boolean;

  /**
   * When true, disables 'not glob' and 'not contains' filter options. Use this
   * when the backend (e.g., structured query) doesn't support negated glob
   * operations.
   *
   * TODO(stevegolton/mazyner): Remove this flag when structured query supports
   * not glob.
   *
   * Default = false.
   */
  readonly structuredQueryCompatMode?: boolean;

  /**
   * Callback that receives the DataGrid API when the grid is ready.
   * Allows parent components to programmatically export data.
   */
  readonly onReady?: (api: DataGridApi) => void;
}

export interface DataGridApi {
  /**
   * Export all filtered and sorted data from the grid.
   * @param format The format to export in
   * @returns Promise<string> The formatted data as a string
   */
  exportData(format: 'tsv' | 'json' | 'markdown'): Promise<string>;

  /**
   * Get the total number of rows in the current filtered dataset.
   * @returns The total row count
   */
  getRowCount(): number | undefined;
}

function getOrCreateDataSource(data: DataSource | readonly Row[]): DataSource {
  if ('notify' in data) {
    return data;
  } else {
    return new InMemoryDataSource(data);
  }
}

/**
 * Context passed to flat (non-pivot) grid builders.
 */
interface FlatGridBuildContext {
  readonly attrs: DataGridAttrs;
  readonly schema: SchemaRegistry;
  readonly rootSchema: string;
  readonly datasource: DataSource;
  readonly result: DataSource['rows'];
  readonly columnInfoCache: Map<string, ReturnType<typeof getColumnInfo>>;
  readonly structuredQueryCompatMode: boolean;
  readonly enablePivotControls: boolean;
}

/**
 * Context passed to pivot grid builders.
 */
interface PivotGridBuildContext {
  readonly attrs: DataGridAttrs;
  readonly schema: SchemaRegistry;
  readonly rootSchema: string;
  readonly datasource: DataSource;
  readonly result: DataSource['rows'];
  readonly pivot: Pivot;
  readonly structuredQueryCompatMode: boolean;
  readonly enablePivotControls: boolean;
}

export class DataGrid implements m.ClassComponent<DataGridAttrs> {
  // Internal model state
  private columns: readonly Column[] = [];
  private filters: readonly Filter[] = [];
  private pivot?: Pivot;

  // Track pagination state from virtual scrolling
  private paginationOffset: number = 0;
  private paginationLimit: number = 100;

  // The grid API instance for column autosizing etc
  private gridApi?: GridApi;

  // Track columns needing distinct values
  private distinctValuesColumns = new Set<string>();

  // Track parameterized columns needing key discovery
  private parameterKeyColumns = new Set<string>();

  oninit({attrs}: m.Vnode<DataGridAttrs>) {
    if (attrs.initialColumns) {
      this.columns = attrs.initialColumns;
    } else {
      this.columns = getDefaultVisibleColumns(
        attrs.schema,
        attrs.rootSchema,
      ).map((field) => ({field}));
    }

    if (attrs.initialFilters) {
      this.filters = attrs.initialFilters;
    }

    if (attrs.initialPivot) {
      this.pivot = attrs.initialPivot;
    }
  }

  view({attrs}: m.Vnode<DataGridAttrs>) {
    const {
      fillHeight,
      className,
      data,
      columns,
      filters,
      pivot,
      schema,
      rootSchema,
      structuredQueryCompatMode = false,
      enablePivotControls = true,
      toolbarItemsLeft,
      toolbarItemsRight,
      showExportButton,
    } = attrs;

    // Update internal state if any are in controlled mode.
    if (columns) this.columns = columns;
    if (filters) this.filters = filters;
    if (pivot) this.pivot = pivot;

    // Collect all fields needed including dependencies
    const visibleFields = this.columns.map((c) => c.field);
    const dependencyFields = new Set<string>();

    // Gather dependency fields from column definitions
    for (const col of this.columns) {
      const colInfo = getColumnInfo(schema, rootSchema, col.field);
      if (colInfo?.dependsOn) {
        for (const dep of colInfo.dependsOn) {
          dependencyFields.add(dep);
        }
      }
    }

    // Also gather dependency fields from pivot columns when in pivot mode
    if (this.pivot) {
      // Check groupBy columns for dependencies
      for (const groupByCol of this.pivot.groupBy) {
        const colInfo = getColumnInfo(schema, rootSchema, groupByCol.field);
        if (colInfo?.dependsOn) {
          for (const dep of colInfo.dependsOn) {
            dependencyFields.add(dep);
          }
        }
      }

      // Check aggregate columns (those with a field) for dependencies
      for (const agg of this.pivot.aggregates ?? []) {
        if (agg.function === 'COUNT') continue;
        const colInfo = getColumnInfo(schema, rootSchema, agg.field);
        if (colInfo?.dependsOn) {
          for (const dep of colInfo.dependsOn) {
            dependencyFields.add(dep);
          }
        }
      }
    }

    // Create columns array with dependencies included
    const columnsWithDeps: readonly Column[] = [
      ...this.columns,
      ...Array.from(dependencyFields)
        .filter((field) => !visibleFields.includes(field))
        .map((field) => ({field})),
    ];

    // Notify the data source of the current model state.
    const datasource = getOrCreateDataSource(data);
    datasource.notify({
      columns: columnsWithDeps,
      filters: this.filters,
      pagination: {
        offset: this.paginationOffset,
        limit: this.paginationLimit,
      },
      pivot: this.pivot,
      distinctValuesColumns: this.distinctValuesColumns,
      parameterKeyColumns: this.parameterKeyColumns,
    });

    // Expose the API
    attrs.onReady?.({
      exportData: async (format) => {
        return await this.formatData(
          datasource,
          schema,
          rootSchema,
          this.pivot,
          format,
        );
      },
      getRowCount: () => {
        return datasource?.rows?.totalRows;
      },
    });

    // Extract the result from the datasource
    const result = datasource.rows;

    // Determine if we're in pivot mode (has groupBy columns and not drilling down)
    const isPivotMode =
      this.pivot !== undefined &&
      this.pivot.groupBy.length > 0 &&
      this.pivot.drillDown === undefined;

    // Build grid columns and rows based on mode
    let gridColumns: GridColumn[];
    let gridRows: m.Children[][];

    if (isPivotMode) {
      // Build context for pivot mode
      const pivotContext: PivotGridBuildContext = {
        attrs,
        schema,
        rootSchema,
        datasource,
        result,
        pivot: this.pivot!,
        structuredQueryCompatMode,
        enablePivotControls,
      };

      gridColumns = this.buildPivotColumns(pivotContext);
      gridRows = this.buildPivotRows(pivotContext);
    } else {
      // Cache column info for all columns once, to avoid repeated lookups
      const columnInfoCache = new Map(
        this.columns.map((col) => [
          col.field,
          getColumnInfo(schema, rootSchema, col.field),
        ]),
      );

      // Build context for flat mode
      const flatContext: FlatGridBuildContext = {
        attrs,
        schema,
        rootSchema,
        datasource,
        result,
        columnInfoCache,
        structuredQueryCompatMode,
        enablePivotControls,
      };

      gridColumns = this.buildFlatColumns(flatContext);
      gridRows = this.buildFlatRows(flatContext);
    }

    return m(
      '.pf-data-grid',
      {
        className: classNames(
          fillHeight && 'pf-data-grid--fill-height',
          className,
        ),
      },
      m(DataGridToolbar, {
        leftItems: toolbarItemsLeft,
        rightItems: [
          toolbarItemsRight,
          showExportButton &&
            m(DataGridExportButton, {
              onExportData: (format) =>
                this.formatData(
                  datasource,
                  schema,
                  rootSchema,
                  this.pivot,
                  format,
                ),
            }),
        ],
        filterChips: this.filters.map((filter, index) =>
          m(GridFilterChip, {
            content: this.formatFilter(filter, schema, rootSchema),
            onRemove: () => this.removeFilter(index, attrs),
          }),
        ),
        drillDown: this.pivot?.drillDown,
        drillDownFields: this.pivot?.groupBy.map(({field}) => {
          const colInfo = getColumnInfo(schema, rootSchema, field);
          const titleParts = colInfo?.titleParts ?? field.split('.');
          const rawValue = this.pivot?.drillDown?.[field];
          return {
            title: buildColumnTitle(titleParts),
            value: formatChipValue(rawValue, colInfo?.cellFormatter),
          };
        }),
        onExitDrillDown: () => this.exitDrillDown(attrs),
      }),
      m(LinearProgress, {
        className: 'pf-data-grid__loading',
        state: datasource.isLoading ? 'indeterminate' : 'none',
      }),
      m(Grid, {
        className: 'pf-data-grid__table',
        columns: gridColumns,
        rowData: {
          data: gridRows,
          total: result?.totalRows ?? 0,
          offset: Math.max(result?.rowOffset ?? 0, this.paginationOffset),
          onLoadData: (offset, limit) => {
            this.paginationOffset = offset;
            this.paginationLimit = limit;
            m.redraw();
          },
        },
        virtualization: {
          rowHeightPx: 25,
        },
        fillHeight: true,
        onColumnReorder: (from, to, position) => {
          if (isPivotMode) {
            this.handlePivotColumnReorder(from, to, position, attrs);
          } else {
            this.reorderColumns(from, to, position, attrs);
          }
        },
        onReady: (api) => {
          this.gridApi = api;
        },
        emptyState:
          result?.totalRows === 0 &&
          !datasource.isLoading &&
          m(
            EmptyState,
            {
              title:
                this.filters.length > 0
                  ? 'No results match your filters'
                  : 'No data available',
              fillHeight: true,
            },
            this.filters.length > 0 &&
              m(Button, {
                variant: ButtonVariant.Filled,
                icon: Icons.FilterOff,
                label: 'Clear filters',
                onclick: () => this.clearFilters(attrs),
              }),
          ),
      }),
    );
  }

  private updateSort(
    field: string,
    direction: SortDirection | undefined,
    attrs: DataGridAttrs,
  ): void {
    const newColumns = this.columns.map((c) =>
      c.field === field ? {...c, sort: direction} : {...c, sort: undefined},
    );
    this.columns = newColumns;
    attrs.onColumnsChanged?.(newColumns);
  }

  private addFilter(filter: Filter, attrs: DataGridAttrs): void {
    attrs.onFilterAdd?.(filter);
    const newFilters = [...this.filters, filter];
    this.filters = newFilters;
    attrs.onFiltersChanged?.(newFilters);
  }

  private removeColumn(field: string, attrs: DataGridAttrs): void {
    const newColumns = this.columns.filter((c) => c.field !== field);
    this.columns = newColumns;
    attrs.onColumnsChanged?.(newColumns);
  }

  private clearFilters(attrs: DataGridAttrs): void {
    this.filters = [];
    attrs.onFiltersChanged?.([]);
  }

  private removeFilter(index: number, attrs: DataGridAttrs): void {
    const newFilters = this.filters.filter((_, i) => i !== index);
    this.filters = newFilters;
    attrs.onFiltersChanged?.(newFilters);
  }

  private addColumn(
    field: string,
    attrs: DataGridAttrs,
    afterIndex?: number,
  ): void {
    const newColumns = [...this.columns];
    const insertIndex =
      afterIndex !== undefined ? afterIndex + 1 : newColumns.length;
    newColumns.splice(insertIndex, 0, {field});
    this.columns = newColumns;
    attrs.onColumnsChanged?.(newColumns);
  }

  private reorderColumns(
    from: string | number | undefined,
    to: string | number | undefined,
    position: 'before' | 'after',
    attrs: DataGridAttrs,
  ): void {
    if (typeof from !== 'string' || typeof to !== 'string') return;
    if (from === to) return;

    const fields = this.columns.map((c) => c.field);
    const fromIndex = fields.indexOf(from);
    const toIndex = fields.indexOf(to);
    if (fromIndex === -1 || toIndex === -1) return;

    const newFields = [...fields];
    newFields.splice(fromIndex, 1);
    let insertIndex = toIndex;
    if (fromIndex < toIndex) insertIndex--;
    if (position === 'after') insertIndex++;
    newFields.splice(insertIndex, 0, from);

    const newColumns = newFields.map(
      (field) => this.columns.find((c) => c.field === field)!,
    );
    this.columns = newColumns;
    attrs.onColumnsChanged?.(newColumns);
  }

  private updateColumnAggregate(
    field: string,
    aggregate: AggregateFunction | undefined,
    attrs: DataGridAttrs,
  ): void {
    const newColumns = this.columns.map((c) =>
      c.field === field ? {...c, aggregate} : c,
    );
    this.columns = newColumns;
    attrs.onColumnsChanged?.(newColumns);
  }

  // ===========================================================================
  // Pivot mode methods
  // ===========================================================================

  private updatePivotGroupBySort(
    index: number,
    direction: SortDirection | undefined,
    attrs: DataGridAttrs,
  ): void {
    if (!this.pivot) return;

    const newGroupBy = this.pivot.groupBy.map((col, i) => {
      const field = col.field;
      if (i === index) {
        return {field, sort: direction};
      }
      // Clear sort on other groupBy columns
      return {field, sort: undefined};
    });

    // Clear sort on aggregate columns when sorting by groupBy
    const newAggregates = this.pivot.aggregates?.map((agg) => ({
      ...agg,
      sort: undefined,
    }));

    const newPivot: Pivot = {
      ...this.pivot,
      groupBy: newGroupBy,
      aggregates: newAggregates,
    };
    this.pivot = newPivot;
    attrs.onPivotChanged?.(newPivot);
  }

  private updatePivotAggregateSort(
    index: number,
    direction: SortDirection | undefined,
    attrs: DataGridAttrs,
  ): void {
    if (!this.pivot?.aggregates) return;

    // Clear sort on groupBy columns when sorting by aggregate
    const newGroupBy = this.pivot.groupBy.map((col) => {
      const field = col.field;
      return {field, sort: undefined};
    });

    const newAggregates = this.pivot.aggregates.map((agg, i) => {
      if (i === index) {
        return {...agg, sort: direction};
      }
      // Clear sort on other aggregate columns
      return {...agg, sort: undefined};
    });

    const newPivot: Pivot = {
      ...this.pivot,
      groupBy: newGroupBy,
      aggregates: newAggregates,
    };
    this.pivot = newPivot;
    attrs.onPivotChanged?.(newPivot);
  }

  private removeGroupByColumn(index: number, attrs: DataGridAttrs): void {
    if (!this.pivot) return;

    const newGroupBy = this.pivot.groupBy.filter((_, i) => i !== index);

    // If no more groupBy columns, exit pivot mode
    if (newGroupBy.length === 0) {
      this.pivot = undefined;
      attrs.onPivotChanged?.(undefined);
      return;
    }

    const newPivot: Pivot = {...this.pivot, groupBy: newGroupBy};
    this.pivot = newPivot;
    attrs.onPivotChanged?.(newPivot);
  }

  private drillDown(row: Row, attrs: DataGridAttrs): void {
    if (!this.pivot) return;

    // Build drill-down filter from groupBy column values
    const drillDownRow: Row = {};
    for (const groupByCol of this.pivot.groupBy) {
      const field = groupByCol.field;
      drillDownRow[field] = row[field];
    }

    const newPivot: Pivot = {...this.pivot, drillDown: drillDownRow};
    this.pivot = newPivot;
    attrs.onPivotChanged?.(newPivot);
  }

  private exitDrillDown(attrs: DataGridAttrs): void {
    if (!this.pivot?.drillDown) return;

    const newPivot: Pivot = {...this.pivot, drillDown: undefined};
    this.pivot = newPivot;
    attrs.onPivotChanged?.(newPivot);
  }

  private removeAggregateColumn(index: number, attrs: DataGridAttrs): void {
    if (!this.pivot?.aggregates) return;

    const newAggregates = this.pivot.aggregates.filter((_, i) => i !== index);
    const newPivot: Pivot = {...this.pivot, aggregates: newAggregates};
    this.pivot = newPivot;
    attrs.onPivotChanged?.(newPivot);
  }

  private convertAggregateToGroupBy(
    aggregateIndex: number,
    field: string,
    attrs: DataGridAttrs,
  ): void {
    if (!this.pivot?.aggregates) return;

    // Remove the aggregate and add its field to groupBy
    const newAggregates = this.pivot.aggregates.filter(
      (_, i) => i !== aggregateIndex,
    );
    const newGroupBy = [...this.pivot.groupBy, {field}];

    const newPivot: Pivot = {
      ...this.pivot,
      groupBy: newGroupBy,
      aggregates: newAggregates,
    };
    this.pivot = newPivot;
    attrs.onPivotChanged?.(newPivot);
  }

  private groupByColumn(field: string, attrs: DataGridAttrs): void {
    // Create a new pivot with this column as the groupBy
    // Add all other visible columns as ANY aggregates so they remain visible
    const aggregates: AggregateColumn[] = [{function: 'COUNT'}];
    for (const col of this.columns) {
      if (col.field !== field) {
        aggregates.push({function: 'ANY', field: col.field});
      }
    }

    const newPivot: Pivot = {
      groupBy: [{field}],
      aggregates,
    };
    this.pivot = newPivot;
    attrs.onPivotChanged?.(newPivot);
  }

  private addGroupByColumn(
    field: string,
    attrs: DataGridAttrs,
    afterIndex?: number,
  ): void {
    if (!this.pivot) return;

    // Check if already in groupBy
    const existingFields = this.pivot.groupBy.map(({field}) => field);
    if (existingFields.includes(field)) return;

    const newGroupBy = [...this.pivot.groupBy];
    const insertIndex =
      afterIndex !== undefined ? afterIndex + 1 : newGroupBy.length;
    newGroupBy.splice(insertIndex, 0, {field});

    const newPivot: Pivot = {...this.pivot, groupBy: newGroupBy};
    this.pivot = newPivot;
    attrs.onPivotChanged?.(newPivot);
  }

  private addAggregateColumn(
    func: AggregateFunction | 'COUNT',
    field: string | undefined,
    attrs: DataGridAttrs,
    afterIndex?: number,
  ): void {
    if (!this.pivot) return;

    const existingAggregates = this.pivot.aggregates ?? [];

    // Check if this aggregate already exists (same function + field)
    const isDuplicate = existingAggregates.some((agg) => {
      if (agg.function !== func) return false;
      const aggField = 'field' in agg ? agg.field : undefined;
      return aggField === field;
    });
    if (isDuplicate) return;

    // Build the new aggregate
    const newAggregate =
      func === 'COUNT'
        ? {function: 'COUNT' as const}
        : {function: func, field: field!};

    const newAggregates = [...existingAggregates];
    const insertIndex =
      afterIndex !== undefined ? afterIndex + 1 : newAggregates.length;
    newAggregates.splice(insertIndex, 0, newAggregate);

    const newPivot: Pivot = {...this.pivot, aggregates: newAggregates};
    this.pivot = newPivot;
    attrs.onPivotChanged?.(newPivot);
  }

  private changeAggregateFunction(
    index: number,
    newFunc: AggregateFunction | 'COUNT',
    attrs: DataGridAttrs,
  ): void {
    if (!this.pivot?.aggregates) return;

    const existingAgg = maybeUndefined(this.pivot.aggregates[index]);
    if (!existingAgg) return;

    // Build the updated aggregate, preserving sort if present
    const updatedAggregate =
      newFunc === 'COUNT'
        ? {function: 'COUNT' as const, sort: existingAgg.sort}
        : {
            function: newFunc,
            field: 'field' in existingAgg ? existingAgg.field : undefined!,
            sort: existingAgg.sort,
          };

    // If changing to COUNT, we lose the field, so only allow for existing COUNT
    // or create a fieldless aggregate
    if (newFunc === 'COUNT' || 'field' in existingAgg) {
      const newAggregates = [...this.pivot.aggregates];
      newAggregates[index] = updatedAggregate;

      const newPivot: Pivot = {...this.pivot, aggregates: newAggregates};
      this.pivot = newPivot;
      attrs.onPivotChanged?.(newPivot);
    }
  }

  private handlePivotColumnReorder(
    from: string | number | undefined,
    to: string | number | undefined,
    position: 'before' | 'after',
    attrs: DataGridAttrs,
  ): void {
    if (typeof from !== 'string' || typeof to !== 'string') return;
    if (from === to) return;

    // Parse column keys to determine type and index
    const parseKey = (
      key: string,
    ): {type: 'groupby' | 'aggregate'; index: number} | undefined => {
      if (key.startsWith('groupby:')) {
        const field = key.slice('groupby:'.length);
        const index = this.pivot?.groupBy.findIndex(
          (col) => col.field === field,
        );
        if (index !== undefined && index >= 0) {
          return {type: 'groupby', index};
        }
      } else if (key.startsWith('aggregate:')) {
        // Key format: aggregate:FUNC:FIELD (e.g., aggregate:SUM:duration)
        const rest = key.slice('aggregate:'.length);
        const colonIdx = rest.indexOf(':');
        if (colonIdx >= 0) {
          const func = rest.slice(0, colonIdx);
          const field = rest.slice(colonIdx + 1);
          const index = this.pivot?.aggregates?.findIndex((agg) => {
            if (agg.function !== func) return false;
            const aggField = 'field' in agg ? agg.field : '';
            return aggField === field;
          });
          if (index !== undefined && index >= 0) {
            return {type: 'aggregate', index};
          }
        }
      }
      return undefined;
    };

    const fromParsed = parseKey(from);
    const toParsed = parseKey(to);

    if (!fromParsed || !toParsed) return;
    if (fromParsed.type !== toParsed.type) return; // Can't reorder across types

    if (fromParsed.type === 'groupby') {
      this.reorderGroupByColumns(
        fromParsed.index,
        toParsed.index,
        position,
        attrs,
      );
    } else {
      this.reorderAggregateColumns(
        fromParsed.index,
        toParsed.index,
        position,
        attrs,
      );
    }
  }

  private reorderGroupByColumns(
    fromIndex: number,
    toIndex: number,
    position: 'before' | 'after',
    attrs: DataGridAttrs,
  ): void {
    if (!this.pivot) return;

    const groupBy = [...this.pivot.groupBy];
    const [removed] = groupBy.splice(fromIndex, 1);
    let insertIndex = toIndex;
    if (fromIndex < toIndex) insertIndex--;
    if (position === 'after') insertIndex++;
    groupBy.splice(insertIndex, 0, removed);

    const newPivot: Pivot = {...this.pivot, groupBy};
    this.pivot = newPivot;
    attrs.onPivotChanged?.(newPivot);
  }

  private reorderAggregateColumns(
    fromIndex: number,
    toIndex: number,
    position: 'before' | 'after',
    attrs: DataGridAttrs,
  ): void {
    if (!this.pivot?.aggregates) return;

    const aggregates = [...this.pivot.aggregates];
    const [removed] = aggregates.splice(fromIndex, 1);
    let insertIndex = toIndex;
    if (fromIndex < toIndex) insertIndex--;
    if (position === 'after') insertIndex++;
    aggregates.splice(insertIndex, 0, removed);

    const newPivot: Pivot = {...this.pivot, aggregates};
    this.pivot = newPivot;
    attrs.onPivotChanged?.(newPivot);
  }

  // ===========================================================================
  // Grid builders
  // ===========================================================================

  /**
   * Builds grid columns for flat (non-pivot) mode.
   */
  private buildFlatColumns(ctx: FlatGridBuildContext): GridColumn[] {
    const {
      attrs,
      schema,
      rootSchema,
      datasource,
      columnInfoCache,
      structuredQueryCompatMode,
      enablePivotControls,
    } = ctx;

    // Find the current sort direction (if any column is sorted)
    const currentSortDirection =
      this.columns.find((c) => c.sort)?.sort ?? 'ASC';

    const columns: GridColumn[] = this.columns.map((col, colIndex) => {
      const {field, sort, aggregate} = col;

      // Get column info from cache
      const colInfo = columnInfoCache.get(field);

      // Build column title with chevron separators
      const titleParts = colInfo?.titleParts ?? field.split('.');
      const titleContent = buildColumnTitle(titleParts);

      // Build menu items
      const columnType = colInfo?.columnType;
      const menuItems: m.Children[] = [
        renderSortMenuItems(sort, (direction) =>
          this.updateSort(field, direction, attrs),
        ),
        m(MenuDivider),
        m(FilterMenu, {
          columnType,
          structuredQueryCompatMode,
          distinctValues: datasource.distinctValues?.get(field),
          valueFormatter: (v) => colInfo?.cellFormatter?.(v, {}) ?? String(v),
          onFilterAdd: (filter) => this.addFilter({field, ...filter}, attrs),
          onRequestDistinctValues: () => this.distinctValuesColumns.add(field),
          onDismissDistinctValues: () =>
            this.distinctValuesColumns.delete(field),
        }),
        m(MenuDivider),
        this.gridApi &&
          m(MenuItem, {
            label: 'Fit to content',
            icon: 'fit_width',
            onclick: () => this.gridApi!.autoFitColumn(field),
          }),
        m(MenuDivider),
        m(ColumnMenu, {
          canRemove: this.columns.length > 1,
          onRemove: () => this.removeColumn(field, attrs),
          schema,
          rootSchema,
          visibleColumns: this.columns.map((c) => c.field),
          onAddColumn: (newField) => this.addColumn(newField, attrs, colIndex),
          dataSource: datasource,
          parameterKeyColumns: this.parameterKeyColumns,
        }),
        m(MenuDivider),
        enablePivotControls &&
          m(MenuItem, {
            label: 'Group by this column',
            icon: 'pivot_table_chart',
            onclick: () => this.groupByColumn(field, attrs),
          }),
        m(MenuDivider),
        // Summary menu - show available functions based on column type
        // Filter out ANY since it only makes sense in pivot mode (arbitrary value from group)
        (() => {
          const funcs = getAggregateFunctionsForColumnType(columnType).filter(
            (f) => f !== 'ANY',
          );
          if (funcs.length === 0) return undefined;
          return m(MenuItem, {label: 'Summary function', icon: 'functions'}, [
            m(MenuItem, {
              label: 'None',
              disabled: aggregate === undefined,
              onclick: () =>
                this.updateColumnAggregate(field, undefined, attrs),
            }),
            ...funcs.map((func) =>
              m(MenuItem, {
                label: func,
                disabled: func === aggregate,
                onclick: () => this.updateColumnAggregate(field, func, attrs),
              }),
            ),
          ]);
        })(),
        m(MenuDivider),
        m(ColumnInfoMenu, {field, colInfo, aggregateFunc: aggregate}),
      ];

      // Build subContent showing grand total if column has an aggregate
      let subContent: m.Children;
      if (aggregate) {
        const totalValue = datasource.aggregateTotals?.get(field);
        const isLoading = totalValue === undefined;
        // Don't show grand total for ANY aggregation (it's just an arbitrary value)
        let totalContent: m.Children;
        if (!isLoading && colInfo?.cellRenderer) {
          const rendered = colInfo.cellRenderer(totalValue, {});
          totalContent = isCellRenderResult(rendered)
            ? rendered.content
            : rendered;
        } else if (!isLoading) {
          totalContent = renderCell(totalValue, field);
        }
        subContent =
          aggregate === 'ANY'
            ? m(AggregationCell, {symbol: aggregate})
            : m(AggregationCell, {symbol: aggregate, isLoading}, totalContent);
      }

      return {
        key: field,
        header: m(
          GridHeaderCell,
          {
            sort,
            hintSortDirection: currentSortDirection,
            onSort: (direction) => this.updateSort(field, direction, attrs),
            menuItems,
            subContent,
          },
          titleContent,
        ),
        reorderable: {reorderGroup: '__datagrid_columns__'},
      };
    });

    return columns;
  }

  /**
   * Builds grid rows for flat (non-pivot) mode.
   */
  private buildFlatRows(ctx: FlatGridBuildContext): m.Children[][] {
    const {attrs, result, columnInfoCache} = ctx;

    if (result === undefined) return [];

    // Find the intersection of rows between what we have and what is required
    // and only render those.
    const start = Math.max(result.rowOffset, this.paginationOffset);

    const rowIndices = Array.from(
      {length: this.paginationLimit},
      (_, i) => i + start,
    );

    return rowIndices
      .map((index) => {
        const row = result.rows[index - result.rowOffset];
        if (row === undefined) return undefined;

        return this.columns.map((col) => {
          const {field} = col;
          const value = row[field];
          const colInfo = columnInfoCache.get(field);
          const cellRenderer =
            colInfo?.cellRenderer ?? ((v: SqlValue) => renderCell(v, field));
          const rendered = cellRenderer(value, row);
          const isRich = isCellRenderResult(rendered);

          return m(
            GridCell,
            {
              align: isRich ? rendered.align ?? 'left' : getAligment(value),
              nullish: isRich
                ? rendered.nullish ?? value === null
                : value === null,
              menuItems: [
                m(CellFilterMenu, {
                  value,
                  onFilterAdd: (filter) =>
                    this.addFilter({field, ...filter}, attrs),
                }),
              ],
            },
            isRich ? rendered.content : rendered,
          );
        });
      })
      .filter(exists);
  }

  /**
   * Builds grid columns for pivot mode.
   * Pivot mode has two types of columns:
   * 1. GroupBy columns - the columns we're grouping by
   * 2. Aggregate columns - SUM, COUNT, AVG, etc.
   */
  private buildPivotColumns(ctx: PivotGridBuildContext): GridColumn[] {
    const {
      attrs,
      schema,
      rootSchema,
      datasource,
      pivot,
      structuredQueryCompatMode,
      enablePivotControls,
    } = ctx;

    const columns: GridColumn[] = [];

    // Get current groupBy field names for disabling already-grouped columns
    const currentGroupByFields = pivot.groupBy.map(({field}) => field);

    // Find the current sort direction (if any column is sorted)
    const currentSortDirection =
      pivot.groupBy.find((col) => col.sort)?.sort ??
      pivot.aggregates?.find((col) => col.sort)?.sort ??
      'ASC';

    // Build groupBy columns
    for (let i = 0; i < pivot.groupBy.length; i++) {
      const groupByCol = pivot.groupBy[i];
      const field = groupByCol.field;
      const sort = groupByCol.sort;
      const isLastGroupBy = i === pivot.groupBy.length - 1;

      // Get column info from schema
      const colInfo = getColumnInfo(schema, rootSchema, field);
      const titleParts = colInfo?.titleParts ?? field.split('.');
      const titleContent = buildColumnTitle(titleParts);

      // Build menu items for groupBy column
      const columnType = colInfo?.columnType;
      const menuItems: m.Children[] = [
        renderSortMenuItems(sort, (direction) =>
          this.updatePivotGroupBySort(i, direction, attrs),
        ),
        m(MenuDivider),
        m(FilterMenu, {
          columnType,
          structuredQueryCompatMode,
          distinctValues: datasource.distinctValues?.get(field),
          valueFormatter: (v) => colInfo?.cellFormatter?.(v, {}) ?? String(v),
          onFilterAdd: (filter) => this.addFilter({field, ...filter}, attrs),
          onRequestDistinctValues: () => this.distinctValuesColumns.add(field),
          onDismissDistinctValues: () =>
            this.distinctValuesColumns.delete(field),
        }),
        m(MenuDivider),
        enablePivotControls &&
          m(ColumnMenu, {
            schema,
            rootSchema,
            visibleColumns: currentGroupByFields,
            onAddColumn: (newField) =>
              this.addGroupByColumn(newField, attrs, i),
            dataSource: datasource,
            parameterKeyColumns: this.parameterKeyColumns,
            canRemove: true,
            onRemove: () => this.removeGroupByColumn(i, attrs),
            removeLabel: 'Remove group by',
            addLabel: 'Add group by',
          }),
        m(MenuDivider),
        m(ColumnInfoMenu, {field, colInfo}),
      ];

      columns.push({
        key: `groupby:${field}`,
        header: m(
          GridHeaderCell,
          {
            hintSortDirection: currentSortDirection,
            sort,
            onSort: (direction) =>
              this.updatePivotGroupBySort(i, direction, attrs),
            menuItems,
            className: classNames(
              'pf-data-grid__groupby-column',
              isLastGroupBy && 'pf-data-grid__groupby-column--last',
            ),
          },
          titleContent,
        ),
        reorderable: {reorderGroup: '__pivot_groupby__'},
      });
    }

    if (enablePivotControls) {
      columns.push({
        key: '__drilldown__',
        header: m(GridHeaderCell, {
          className: classNames('pf-datagrid__dd'),
        }),
        widthPx: 24,
      });
    }

    // Build aggregate columns
    const aggregates = pivot.aggregates ?? [];
    for (let i = 0; i < aggregates.length; i++) {
      const agg = aggregates[i];
      const sort = agg.sort;
      const alias = getAggregateAlias(agg);

      // Build title from field or "COUNT" for count-only aggregates
      let title: m.Children;
      let colInfo: ReturnType<typeof getColumnInfo> | undefined;
      if ('field' in agg) {
        colInfo = getColumnInfo(schema, rootSchema, agg.field);
        const fieldTitle = colInfo?.titleParts ?? [agg.field];
        title = buildColumnTitle(fieldTitle);
      } else {
        title = 'Count';
      }

      // Build "Change function" submenu for field-based aggregates
      const changeFunctionSubmenu: m.Children =
        'field' in agg
          ? m(
              MenuItem,
              {label: 'Change function', icon: 'functions'},
              getAggregateFunctionsForColumnType(colInfo?.columnType).map(
                (func) =>
                  m(MenuItem, {
                    label: func,
                    disabled: func === agg.function,
                    onclick: () => this.changeAggregateFunction(i, func, attrs),
                  }),
              ),
            )
          : undefined;

      // Build "Group by this" menu item for field-based aggregates
      const groupByThisMenuItem =
        'field' in agg
          ? m(MenuItem, {
              label: 'Group by this column',
              icon: 'pivot_table_chart',
              // Disable if already in groupBy
              disabled: currentGroupByFields.includes(agg.field),
              onclick: () =>
                this.convertAggregateToGroupBy(i, agg.field, attrs),
            })
          : undefined;

      // Build menu items for aggregate column
      const menuItems: m.Children[] = [
        renderSortMenuItems(sort, (direction) =>
          this.updatePivotAggregateSort(i, direction, attrs),
        ),
        m(MenuDivider),
        enablePivotControls && [
          changeFunctionSubmenu,
          groupByThisMenuItem,
          m(AggregateMenu, {
            schema,
            rootSchema,
            existingAggregates: pivot.aggregates,
            onAddAggregate: (func, aggField) =>
              this.addAggregateColumn(func, aggField, attrs, i),
          }),
          m(MenuItem, {
            label: 'Remove column',
            icon: Icons.Remove,
            onclick: () => this.removeAggregateColumn(i, attrs),
          }),
        ],
        m(MenuDivider),
        m(ColumnInfoMenu, {
          field: 'field' in agg ? agg.field : alias,
          colInfo,
          aggregateFunc: agg.function,
        }),
      ];

      // Build subContent showing grand total with aggregate symbol
      // Don't show grand total for ANY aggregation (it's just an arbitrary value)
      const aggregateTotalValue = datasource.aggregateTotals?.get(alias);
      const symbol = agg.function;
      const isLoading = aggregateTotalValue === undefined;
      let aggTotalContent: m.Children;
      if (!isLoading && colInfo?.cellRenderer) {
        const rendered = colInfo.cellRenderer(aggregateTotalValue, {});
        aggTotalContent = isCellRenderResult(rendered)
          ? rendered.content
          : rendered;
      } else if (!isLoading) {
        aggTotalContent = renderCell(aggregateTotalValue, alias);
      }
      const subContent =
        agg.function !== 'ANY'
          ? m(AggregationCell, {symbol, isLoading}, aggTotalContent)
          : m(AggregationCell, {symbol});

      columns.push({
        key: `aggregate:${agg.function}:${'field' in agg ? agg.field : ''}`,
        header: m(
          GridHeaderCell,
          {
            sort,
            hintSortDirection: currentSortDirection,
            onSort: (direction) =>
              this.updatePivotAggregateSort(i, direction, attrs),
            menuItems,
            subContent,
          },
          title,
        ),
        reorderable: {reorderGroup: '__pivot_aggregate__'},
      });
    }

    return columns;
  }

  /**
   * Builds grid rows for pivot mode.
   * Each row contains values for groupBy columns and aggregate columns.
   */
  private buildPivotRows(ctx: PivotGridBuildContext): m.Children[][] {
    const {attrs, schema, rootSchema, result, pivot, enablePivotControls} = ctx;

    if (result === undefined) return [];

    // Find the intersection of rows between what we have and what is required
    // and only render those.
    const start = Math.max(result.rowOffset, this.paginationOffset);

    const rowIndices = Array.from(
      {length: this.paginationLimit},
      (_, i) => i + start,
    );

    const aggregates = pivot.aggregates ?? [];

    return rowIndices
      .map((index) => {
        const row = result.rows[index - result.rowOffset];
        if (row === undefined) return undefined;

        const cells: m.Children[] = [];

        // Render groupBy columns
        for (let i = 0; i < pivot.groupBy.length; i++) {
          const groupByCol = pivot.groupBy[i];
          const field = groupByCol.field;
          const value = row[field];

          // Get cell renderer from schema
          const colInfo = getColumnInfo(schema, rootSchema, field);
          const cellRenderer =
            colInfo?.cellRenderer ?? ((v: SqlValue) => renderCell(v, field));
          const rendered = cellRenderer(value, row);
          const isRich = isCellRenderResult(rendered);

          cells.push(
            m(
              GridCell,
              {
                align: isRich ? rendered.align ?? 'left' : getAligment(value),
                nullish: isRich
                  ? rendered.nullish ?? value === null
                  : value === null,
                className: 'pf-data-grid__groupby-column',
                menuItems: [
                  enablePivotControls && [
                    m(MenuItem, {
                      label: 'Drill down',
                      icon: 'zoom_in',
                      onclick: () => this.drillDown(row, attrs),
                    }),
                    m(MenuDivider),
                  ],
                  m(CellFilterMenu, {
                    value,
                    onFilterAdd: (filter) =>
                      this.addFilter({field, ...filter}, attrs),
                  }),
                ],
              },
              isRich ? rendered.content : rendered,
            ),
          );
        }

        // Only add drill-down cell if pivot controls are enabled
        if (enablePivotControls) {
          const drillDownCell = m(
            '.pf-datagrid__dd-cell',
            {className: 'pf-datagrid__dd'},
            m(Button, {
              className:
                'pf-visible-on-row-hover pf-datagrid__drilldown-button',
              icon: Icons.GoTo,
              rounded: true,
              title: 'Drill down into this group',
              fillWidth: true,
              onclick: () => {
                this.drillDown(row, attrs);
              },
            }),
          );

          cells.push(drillDownCell);
        }

        // Render aggregate columns
        for (const agg of aggregates) {
          const alias = getAggregateAlias(agg);
          const value = row[alias];

          // For aggregates with a field, we can use the field's cell renderer
          let cellRenderer;
          if ('field' in agg) {
            const aggColInfo = getColumnInfo(schema, rootSchema, agg.field);
            cellRenderer = aggColInfo?.cellRenderer;
          }

          const rendered = cellRenderer?.(value, row);
          const isRich = rendered !== undefined && isCellRenderResult(rendered);

          cells.push(
            m(
              GridCell,
              {
                // Default to 'right' for aggregates, but allow override
                align: isRich ? rendered.align ?? 'left' : getAligment(value),
                nullish: isRich
                  ? rendered.nullish ?? value === null
                  : value === null,
              },
              isRich ? rendered.content : rendered ?? String(value ?? ''),
            ),
          );
        }

        return cells;
      })
      .filter(exists);
  }

  private async formatData(
    dataSource: DataSource,
    schema: SchemaRegistry | undefined,
    rootSchema: string | undefined,
    pivot: Pivot | undefined,
    format: 'tsv' | 'json' | 'markdown' = 'tsv',
  ): Promise<string> {
    // Get all rows from the data source
    const rows = await dataSource.exportData();

    // Determine which columns to export based on mode
    let columns: ReadonlyArray<string>;
    let columnNames: Record<string, string>;

    // Check if we're in pivot mode (has groupBy columns and not drilling down)
    const isPivotMode =
      pivot !== undefined &&
      pivot.groupBy.length > 0 &&
      pivot.drillDown === undefined;

    if (isPivotMode) {
      // In pivot mode, export groupBy fields + aggregate aliases
      columns = [
        ...pivot.groupBy.map(({field}) => field),
        ...(pivot.aggregates ?? []).map((agg) => getAggregateAlias(agg)),
      ];

      // Build custom column names for pivot mode
      columnNames = {};

      // Add groupBy column names
      for (const groupByCol of pivot.groupBy) {
        const field = groupByCol.field;
        const colInfo =
          schema && rootSchema
            ? getColumnInfo(schema, rootSchema, field)
            : undefined;
        columnNames[field] = colInfo?.def.titleString ?? field;
      }

      // Add aggregate column names with function wrapped around title
      for (const agg of pivot.aggregates ?? []) {
        const alias = getAggregateAlias(agg);
        if (agg.function === 'COUNT') {
          columnNames[alias] = 'COUNT';
        } else if ('field' in agg) {
          const colInfo =
            schema && rootSchema
              ? getColumnInfo(schema, rootSchema, agg.field)
              : undefined;
          const fieldTitle = colInfo?.def.titleString ?? agg.field;
          columnNames[alias] = `${agg.function}(${fieldTitle})`;
        }
      }
    } else {
      // In flat mode, export the regular visible columns
      columns = this.columns.map((c) => c.field);
      columnNames = buildColumnNames(schema, rootSchema, columns);
    }

    const formattedRows = formatRows(rows, schema, rootSchema, columns);

    // Format the data based on the requested format
    switch (format) {
      case 'tsv':
        return formatAsTSV([...columns], columnNames, formattedRows);
      case 'json':
        return formatAsJSON(formattedRows);
      case 'markdown':
        return formatAsMarkdown([...columns], columnNames, formattedRows);
    }
  }

  private formatFilter(
    filter: Filter,
    schema: SchemaRegistry,
    rootSchema: string,
  ): m.Children {
    // Resolve column info once for all properties
    const colInfo = getColumnInfo(schema, rootSchema, filter.field);

    // Build column title with chevron separators
    const titleParts = colInfo?.titleParts ?? filter.field.split('.');
    const columnDisplay = buildColumnTitle(titleParts);

    if ('value' in filter) {
      const value = filter.value;
      // Handle array values
      if (Array.isArray(value)) {
        const valueDisplay =
          value.length > 3
            ? `(${value.length} values)`
            : m('span', [
                '(',
                intersperse(
                  value.map((v) => formatChipValue(v, colInfo?.cellFormatter)),
                  ', ',
                ),
                ')',
              ]);
        return [columnDisplay, ' ', filter.op, ' ', valueDisplay];
      }
      return [
        columnDisplay,
        ' ',
        filter.op,
        ' ',
        formatChipValue(value as SqlValue, colInfo?.cellFormatter),
      ];
    } else {
      return [columnDisplay, ' ', filter.op];
    }
  }
}

export function renderCell(value: SqlValue, columnName: string) {
  if (value === undefined) {
    return '';
  } else if (value instanceof Uint8Array) {
    return m(
      Anchor,
      {
        icon: Icons.Download,
        onclick: () =>
          download({
            fileName: `${columnName}.blob`,
            content: value,
          }),
      },
      `Blob (${value.length} bytes)`,
    );
  } else {
    return String(value);
  }
}

function getAligment(value: SqlValue): 'left' | 'right' | 'center' {
  if (value === null) {
    return 'center';
  } else if (isNumeric(value)) {
    return 'right';
  } else {
    return 'left';
  }
}

/**
 * Builds a column title from title parts, interspersing chevron separators.
 */
function buildColumnTitle(titleParts: m.Children[]): m.Children[] {
  return intersperse(
    titleParts,
    m(Icon, {
      icon: 'chevron_right',
      className: 'pf-data-grid__title-separator',
    }),
  );
}

/**
 * Formats a value for display in filter/drill-down chips.
 * Wraps strings in gray quotes, shows null with muted italic styling.
 */
function formatChipValue(
  value: SqlValue | undefined,
  cellFormatter?: (v: SqlValue, row: Row) => m.Children,
): m.Children {
  if (value === null) {
    return m('span.pf-chip-null', 'null');
  }
  if (value === undefined) return '';

  const formatter = cellFormatter ?? defaultValueFormatter;
  const formatted = formatter(value, {});

  if (typeof value === 'string') {
    return m('span', [
      m('span.pf-filter-quote', '"'),
      formatted,
      m('span.pf-filter-quote', '"'),
    ]);
  }
  return formatted;
}

/**
 * Get the alias (key name) used in result rows for an aggregate column.
 * This matches how data sources store aggregate results:
 * - COUNT: '__count__'
 * - Other aggregates: the field name
 */
function getAggregateAlias(agg: AggregateColumn): string {
  if (agg.function === 'COUNT') {
    return '__count__';
  }
  return 'field' in agg ? agg.field : '__unknown__';
}
