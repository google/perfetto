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
import {shortUuid} from '../../../base/uuid';
import {exists, isNumeric, maybeUndefined} from '../../../base/utils';
import {Row, SqlValue} from '../../../trace_processor/query_result';
import {Anchor} from '../../../widgets/anchor';
import {Button, ButtonGroup, ButtonVariant} from '../../../widgets/button';
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
import {
  DataSource,
  DataSourceModel,
  DataSourceRows,
  FlatModel,
  PivotModel,
  TreeModel,
} from './data_source';
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
  DEFAULT_GROUP_DISPLAY,
  Filter,
  GroupPath,
  IdBasedTree,
  Pivot,
  SortDirection,
} from './model';

// Compare two SqlValues for equality, handling nulls, undefined, and different types.
function sqlValuesEqual(a: SqlValue, b: SqlValue): boolean {
  // Normalize undefined to null (SQL has no concept of undefined)
  const normA = a === undefined ? null : a;
  const normB = b === undefined ? null : b;

  if (normA === null && normB === null) return true;
  if (normA === null || normB === null) return false;
  if (normA instanceof Uint8Array && normB instanceof Uint8Array) {
    if (normA.length !== normB.length) return false;
    for (let i = 0; i < normA.length; i++) {
      if (normA[i] !== normB[i]) return false;
    }
    return true;
  }
  if (typeof normA !== typeof normB) return false;
  return normA === normB;
}

// Compare two GroupPaths for equality.
function groupPathsEqual(a: GroupPath, b: GroupPath): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    if (!sqlValuesEqual(a[i], b[i])) return false;
  }
  return true;
}

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
   * Whether columns can be added. Defaults to true.
   */
  readonly canAddColumns?: boolean;

  /**
   * Whether columns can be removed. Defaults to true.
   */
  readonly canRemoveColumns?: boolean;

  /**
   * Custom menu items to add to each column's header menu.
   * Called with the column field; return menu items (e.g., MenuItem components).
   * Items are placed below the "Add column" section.
   */
  readonly addColumnMenuItems?: (field: string) => m.Children;

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
   * ID-based tree configuration for displaying hierarchical data.
   * Uses id/parent_id columns for tree structure.
   * Mutually exclusive with pivot mode.
   */
  readonly tree?: IdBasedTree;

  /**
   * Initial ID-based tree configuration to apply on first load.
   * This is ignored in controlled mode (i.e. when `tree` is provided).
   */
  readonly initialTree?: IdBasedTree;

  /**
   * Callback triggered when the ID-based tree configuration changes.
   * Required for controlled mode - when provided with tree,
   * the parent component becomes responsible for updating the tree prop.
   */
  readonly onTreeChanged?: (tree: IdBasedTree | undefined) => void;

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

  /**
   * Custom message to display in the empty state when there are no rows to show
   */
  readonly emptyStateMessage?: string;
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
  if ('useRows' in data) {
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
  readonly rowsResult: DataSourceRows;
  readonly aggregateSummaries?: Row;
  readonly columnInfoCache: Map<string, ReturnType<typeof getColumnInfo>>;
  readonly structuredQueryCompatMode: boolean;
  readonly enablePivotControls: boolean;
  // ID-based tree mode - if set, one column displays as a tree with chevrons
  readonly tree?: IdBasedTree;
  readonly columnsAreMutable: boolean;
  readonly filtersAreMutable: boolean;
}

/**
 * Context passed to pivot grid builders.
 */
interface PivotGridBuildContext {
  readonly attrs: DataGridAttrs;
  readonly schema: SchemaRegistry;
  readonly rootSchema: string;
  readonly datasource: DataSource;
  readonly rowsResult: DataSourceRows;
  readonly aggregateSummaries?: Row;
  readonly pivot: Pivot;
  readonly structuredQueryCompatMode: boolean;
  readonly enablePivotControls: boolean;
  readonly filtersAreMutable: boolean;
}

export class DataGrid implements m.ClassComponent<DataGridAttrs> {
  // Internal model state
  private columns: readonly Column[] = [];
  private filters: readonly Filter[] = [];
  private pivot?: Pivot;
  private tree?: IdBasedTree;

  // Track pagination state from virtual scrolling
  private paginationOffset: number = 0;
  private paginationLimit: number = 100;

  // The grid API instance for column autosizing etc
  private gridApi?: GridApi;

  oninit({attrs}: m.Vnode<DataGridAttrs>) {
    if (attrs.initialColumns) {
      this.columns = attrs.initialColumns;
    } else {
      this.columns = getDefaultVisibleColumns(
        attrs.schema,
        attrs.rootSchema,
      ).map((field) => ({id: shortUuid(), field}));
    }

    if (attrs.initialFilters) {
      this.filters = attrs.initialFilters;
    }

    if (attrs.initialPivot) {
      this.pivot = attrs.initialPivot;
    }

    if (attrs.initialTree) {
      this.tree = attrs.initialTree;
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
      tree,
      schema,
      rootSchema,
      structuredQueryCompatMode = false,
      enablePivotControls = true,
      toolbarItemsLeft,
      toolbarItemsRight,
      showExportButton,
      emptyStateMessage,
    } = attrs;

    // Update internal state if any are in controlled mode.
    if (columns) this.columns = columns;
    if (filters) this.filters = filters;
    if (pivot) this.pivot = pivot;
    if (tree) this.tree = tree;

    // Determine if we're in tree mode (has id-based tree config)
    const isTreeMode = this.tree !== undefined;

    // Columns are mutable if in uncontrolled mode, or controlled with callback
    const columnsAreMutable = !columns || !!attrs.onColumnsChanged;

    // Filters are mutable if in uncontrolled mode, or controlled with callback
    const filtersAreMutable = !filters || !!attrs.onFiltersChanged;

    // Determine if we're in pivot mode (has groupBy columns and not drilling down)
    const isPivotMode =
      !isTreeMode &&
      this.pivot !== undefined &&
      this.pivot.groupBy.length > 0 &&
      this.pivot.drillDown === undefined;

    // Build the model for data source queries
    const datasource = getOrCreateDataSource(data);
    const model = this.buildDataSourceModel();

    // Fetch data using the slot-like API
    const rowsResult = datasource.useRows(model);
    const aggregateSummariesResult = datasource.useAggregateSummaries(model);

    // Expose the API
    attrs.onReady?.({
      exportData: async (format) => {
        return await this.formatData(
          datasource,
          model,
          schema,
          rootSchema,
          this.pivot,
          format,
        );
      },
      getRowCount: () => {
        return rowsResult.totalRows;
      },
    });

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
        rowsResult,
        aggregateSummaries: aggregateSummariesResult.data,
        pivot: this.pivot!,
        structuredQueryCompatMode,
        enablePivotControls,
        filtersAreMutable,
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
        rowsResult,
        aggregateSummaries: aggregateSummariesResult.data,
        columnInfoCache,
        structuredQueryCompatMode,
        enablePivotControls,
        tree: this.tree,
        columnsAreMutable,
        filtersAreMutable,
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
        leftItems: [
          toolbarItemsLeft,
          this.renderPivotToolbarItems(attrs),
          this.renderTreeToolbarItems(attrs),
        ],
        rightItems: [
          toolbarItemsRight,
          showExportButton &&
            m(DataGridExportButton, {
              onExportData: (format) =>
                this.formatData(
                  datasource,
                  model,
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
            onRemove: filtersAreMutable
              ? () => this.removeFilter(index, attrs)
              : undefined,
          }),
        ),
        drillDownFields:
          this.pivot?.drillDown &&
          this.pivot.drillDown.map(({field, value}) => {
            const colInfo = getColumnInfo(schema, rootSchema, field);
            const titleParts = colInfo?.titleParts ?? field.split('.');
            return {
              title: buildColumnTitle(titleParts),
              value: formatChipValue(value, colInfo?.cellFormatter),
            };
          }),
        onExitDrillDown: () => this.exitDrillDown(attrs),
      }),
      m(LinearProgress, {
        className: 'pf-data-grid__loading',
        state: rowsResult.isPending ? 'indeterminate' : 'none',
      }),
      m(Grid, {
        className: 'pf-data-grid__table',
        columns: gridColumns,
        rowData: {
          data: gridRows,
          total: rowsResult.totalRows ?? 0,
          offset: Math.max(rowsResult.rowOffset ?? 0, this.paginationOffset),
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
          rowsResult.totalRows === 0 &&
          !rowsResult.isPending &&
          m(
            EmptyState,
            {
              title:
                this.filters.length > 0
                  ? 'No results match your filters'
                  : emptyStateMessage ?? 'No data available',
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

  /**
   * Builds a DataSourceModel from the current state.
   * The model shape depends on whether we're in pivot, tree, or flat mode.
   */
  private buildDataSourceModel(): DataSourceModel {
    // Extract sort from columns (flat mode only)
    const sortedColumn = this.columns.find((c) => c.sort);
    const sort = sortedColumn
      ? {alias: sortedColumn.id, direction: sortedColumn.sort!}
      : undefined;

    // Common base fields
    const baseModel = {
      filters: this.filters,
      pagination: {
        offset: this.paginationOffset,
        limit: this.paginationLimit,
      },
    };

    if (this.pivot && this.pivot.drillDown) {
      // DrillDown mode: use FlatModel with drilldown conditions as filters
      // This shows raw rows filtered to specific group values
      const drillDownFilters: Filter[] = this.pivot.drillDown.map((dd) => ({
        field: dd.field,
        op: dd.value === null ? 'is null' : '=',
        value: dd.value === null ? undefined : dd.value,
      })) as Filter[];

      const flatModel: FlatModel = {
        ...baseModel,
        mode: 'flat',
        sort,
        filters: [...(this.filters ?? []), ...drillDownFilters],
        columns: this.columns
          .map((col) => ({
            field: col.field,
            alias: col.id,
            aggregate: col.aggregate,
          }))
          .sort((a, b) => (a.alias < b.alias ? -1 : a.alias > b.alias ? 1 : 0)),
      };
      return flatModel;
    } else if (this.pivot) {
      // Extract sort from pivot columns (groupBy or aggregates)
      const sortedGroupBy = this.pivot.groupBy.find((c) => c.sort);
      const sortedAggregate = (this.pivot.aggregates ?? []).find((c) => c.sort);
      const sortedPivotCol = sortedGroupBy ?? sortedAggregate;
      const pivotSort = sortedPivotCol
        ? {alias: sortedPivotCol.id, direction: sortedPivotCol.sort!}
        : undefined;

      // Build PivotModel
      const pivotModel: PivotModel = {
        ...baseModel,
        mode: 'pivot',
        sort: pivotSort,
        groupBy: this.pivot.groupBy.map((col) => ({
          field: col.field,
          alias: col.id,
        })),
        aggregates: (this.pivot.aggregates ?? [])
          .map((agg) => {
            if (agg.function === 'COUNT') {
              return {function: 'COUNT' as const, alias: agg.id};
            } else {
              return {function: agg.function, field: agg.field, alias: agg.id};
            }
          })
          .sort((a, b) => (a.alias < b.alias ? -1 : a.alias > b.alias ? 1 : 0)),
        groupDisplay: this.pivot.groupDisplay ?? DEFAULT_GROUP_DISPLAY,
        expandedGroups: this.pivot.expandedGroups,
        collapsedGroups: this.pivot.collapsedGroups,
      };
      return pivotModel;
    } else if (this.tree) {
      // Build TreeModel
      const treeModel: TreeModel = {
        ...baseModel,
        mode: 'tree',
        sort,
        columns: this.columns
          .map((col) => ({
            field: col.field,
            alias: col.id,
          }))
          .sort((a, b) => (a.alias < b.alias ? -1 : a.alias > b.alias ? 1 : 0)),
        tree: this.tree,
      };
      return treeModel;
    } else {
      // Build FlatModel
      const flatModel: FlatModel = {
        ...baseModel,
        mode: 'flat',
        sort,
        columns: this.columns
          .map((col) => ({
            field: col.field,
            alias: col.id,
            aggregate: col.aggregate,
          }))
          .sort((a, b) => (a.alias < b.alias ? -1 : a.alias > b.alias ? 1 : 0)),
      };
      return flatModel;
    }
  }

  private updateSort(
    colId: string,
    direction: SortDirection | undefined,
    attrs: DataGridAttrs,
  ): void {
    const newColumns = this.columns.map((c) =>
      c.id === colId ? {...c, sort: direction} : {...c, sort: undefined},
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

  private removeColumn(colId: string, attrs: DataGridAttrs): void {
    const newColumns = this.columns.filter((c) => c.id !== colId);
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
    const newColumn: Column = {id: shortUuid(), field};
    const newColumns = [...this.columns];
    const insertIndex =
      afterIndex !== undefined ? afterIndex + 1 : newColumns.length;
    newColumns.splice(insertIndex, 0, newColumn);
    this.columns = newColumns;
    attrs.onColumnsChanged?.(newColumns);
  }

  private reorderColumns(
    fromId: string | number | undefined,
    toId: string | number | undefined,
    position: 'before' | 'after',
    attrs: DataGridAttrs,
  ): void {
    if (typeof fromId !== 'string' || typeof toId !== 'string') return;
    if (fromId === toId) return;

    const colIds = this.columns.map((c) => c.id);
    const fromIndex = colIds.indexOf(fromId);
    const toIndex = colIds.indexOf(toId);
    if (fromIndex === -1 || toIndex === -1) return;

    const newColumns = [...this.columns];
    const [removed] = newColumns.splice(fromIndex, 1);
    let insertIndex = toIndex;
    if (fromIndex < toIndex) insertIndex--;
    if (position === 'after') insertIndex++;
    newColumns.splice(insertIndex, 0, removed);

    this.columns = newColumns;
    attrs.onColumnsChanged?.(newColumns);
  }

  private updateColumnAggregate(
    colId: string,
    aggregate: AggregateFunction | undefined,
    attrs: DataGridAttrs,
  ): void {
    const newColumns = this.columns.map((c) =>
      c.id === colId ? {...c, aggregate} : c,
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
      if (i === index) {
        return {...col, sort: direction};
      }
      // Clear sort on other groupBy columns
      return {...col, sort: undefined};
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
      return {...col, sort: undefined};
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

  private drillDown(
    drillDown: readonly {field: string; value: SqlValue}[],
    attrs: DataGridAttrs,
  ): void {
    if (!this.pivot) return;

    const newPivot: Pivot = {...this.pivot, drillDown};
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
    const newGroupBy = [...this.pivot.groupBy, {id: shortUuid(), field}];

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
    const aggregates: AggregateColumn[] = [
      {id: shortUuid(), function: 'COUNT'},
    ];
    for (const col of this.columns) {
      if (col.field !== field) {
        aggregates.push({id: shortUuid(), function: 'ANY', field: col.field});
      }
    }

    const newPivot: Pivot = {
      groupBy: [{id: shortUuid(), field}],
      aggregates,
      groupDisplay: DEFAULT_GROUP_DISPLAY,
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
    newGroupBy.splice(insertIndex, 0, {id: shortUuid(), field});

    // Just update the groupBy - expansion state (expandedIds) is preserved
    const newPivot: Pivot = {
      ...this.pivot,
      groupBy: newGroupBy,
    };
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
        ? {id: shortUuid(), function: 'COUNT' as const}
        : {id: shortUuid(), function: func, field: field!};

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
    newFunc: AggregateFunction,
    attrs: DataGridAttrs,
  ): void {
    if (!this.pivot?.aggregates) return;

    const existingAgg = maybeUndefined(this.pivot.aggregates[index]);
    if (!existingAgg) return;

    if (existingAgg.function === 'COUNT') {
      // Can't change the function of a COUNT aggregate - get out
      return;
    }

    const updatedAggregate = {...existingAgg, function: newFunc};

    // If changing to COUNT, we lose the field, so only allow for existing COUNT
    // or create a fieldless aggregate
    const newAggregates = [...this.pivot.aggregates];
    newAggregates[index] = updatedAggregate;

    const newPivot: Pivot = {...this.pivot, aggregates: newAggregates};
    this.pivot = newPivot;
    attrs.onPivotChanged?.(newPivot);
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

  /**
   * Checks if a group node is currently expanded using its path.
   * Handles both allowlist (expandedGroups) and denylist (collapsedGroups) modes.
   */
  private isGroupExpanded(groupPath: GroupPath): boolean {
    if (!this.pivot) return false;
    // Denylist mode: expanded unless in collapsedGroups
    if (this.pivot.collapsedGroups !== undefined) {
      return !this.pivot.collapsedGroups.some((p) =>
        groupPathsEqual(p, groupPath),
      );
    }
    // Allowlist mode: expanded only if in expandedGroups
    return (
      this.pivot.expandedGroups?.some((p) => groupPathsEqual(p, groupPath)) ??
      false
    );
  }

  /**
   * Toggles the expansion state of a group identified by its path.
   * Used for collapsible rollup rows in multi-level pivot tables.
   * @param groupPath The group values array for the node to toggle
   */
  private toggleExpansion(groupPath: GroupPath, attrs: DataGridAttrs): void {
    if (!this.pivot) return;

    // Handle both allowlist (expandedGroups) and denylist (collapsedGroups) modes
    if (this.pivot.collapsedGroups !== undefined) {
      // Denylist mode: toggle in collapsedGroups
      // In list = collapsed, not in list = expanded
      const currentCollapsed = this.pivot.collapsedGroups;
      const isCollapsed = currentCollapsed.some((p) =>
        groupPathsEqual(p, groupPath),
      );
      let newCollapsed: GroupPath[];
      if (isCollapsed) {
        // Currently collapsed, expand it by removing from list
        newCollapsed = currentCollapsed.filter(
          (p) => !groupPathsEqual(p, groupPath),
        );
      } else {
        // Currently expanded, collapse it by adding to list
        newCollapsed = [...currentCollapsed, groupPath];
      }
      const newPivot: Pivot = {...this.pivot, collapsedGroups: newCollapsed};
      this.pivot = newPivot;
      attrs.onPivotChanged?.(newPivot);
    } else {
      // Allowlist mode: toggle in expandedGroups
      // In list = expanded, not in list = collapsed
      const currentExpanded = this.pivot.expandedGroups ?? [];
      const isExpanded = currentExpanded.some((p) =>
        groupPathsEqual(p, groupPath),
      );
      let newExpanded: GroupPath[];
      if (isExpanded) {
        newExpanded = currentExpanded.filter(
          (p) => !groupPathsEqual(p, groupPath),
        );
      } else {
        newExpanded = [...currentExpanded, groupPath];
      }
      const newPivot: Pivot = {...this.pivot, expandedGroups: newExpanded};
      this.pivot = newPivot;
      attrs.onPivotChanged?.(newPivot);
    }
  }

  /**
   * Renders the pivot toolbar items (expand/collapse buttons and flat/tree toggle).
   * Only shown when in pivot mode and not drilling down.
   */
  private renderPivotToolbarItems(attrs: DataGridAttrs): m.Children {
    if (!this.pivot || this.pivot.drillDown) return null;

    const mode = this.pivot.groupDisplay ?? DEFAULT_GROUP_DISPLAY;
    const isFlat = mode === 'flat';

    return [
      m(Button, {
        icon: 'unfold_more',
        tooltip: 'Expand all groups',
        onclick: () => this.expandAll(attrs),
        disabled: isFlat,
      }),
      m(Button, {
        icon: 'unfold_less',
        tooltip: 'Collapse all groups',
        onclick: () => this.collapseAll(attrs),
        disabled: isFlat,
      }),
      m(
        ButtonGroup,
        m(Button, {
          label: 'Flat',
          icon: 'view_list',
          active: isFlat,
          onclick: () => this.enableFlatMode(attrs),
          tooltip: 'Show all groups in a flat list (no hierarchy)',
        }),
        m(Button, {
          label: 'Tree',
          icon: 'account_tree',
          active: !isFlat,
          onclick: () => this.enableTreeMode(attrs),
          tooltip: 'Show rollups in a hierarchical tree structure',
        }),
      ),
    ];
  }

  private renderTreeToolbarItems(attrs: DataGridAttrs): m.Children {
    if (!this.tree) return null;

    return [
      m(Button, {
        icon: 'unfold_more',
        tooltip: 'Expand all nodes',
        onclick: () => this.expandAll(attrs),
      }),
      m(Button, {
        icon: 'unfold_less',
        tooltip: 'Collapse all nodes',
        onclick: () => this.collapseAll(attrs),
      }),
    ];
  }

  /**
   * Expands all groups by switching to denylist mode with empty collapsedGroups.
   * Empty collapsedGroups = all nodes expanded (nothing is collapsed).
   */
  private expandAll(attrs: DataGridAttrs): void {
    if (this.pivot) {
      // Switch to denylist mode with empty array - all nodes expanded
      const newPivot: Pivot = {
        ...this.pivot,
        expandedGroups: undefined,
        collapsedGroups: [],
      };
      this.pivot = newPivot;
      attrs.onPivotChanged?.(newPivot);
    }

    if (this.tree) {
      // Switch to denylist mode with empty set - all nodes expanded
      const newTree: IdBasedTree = {
        ...this.tree,
        expandedIds: undefined,
        collapsedIds: new Set<bigint>(),
      };
      this.tree = newTree;
      attrs.onTreeChanged?.(newTree);
    }
  }

  /**
   * Collapses all groups by switching to allowlist mode with empty expandedGroups.
   * Empty expandedGroups = all nodes collapsed (nothing is expanded).
   */
  private collapseAll(attrs: DataGridAttrs): void {
    if (this.pivot) {
      // Switch to allowlist mode with empty array - all nodes collapsed
      const newPivot: Pivot = {
        ...this.pivot,
        expandedGroups: [],
        collapsedGroups: undefined,
      };
      this.pivot = newPivot;
      attrs.onPivotChanged?.(newPivot);
    }

    if (this.tree) {
      // Switch to allowlist mode with empty set - all nodes collapsed
      const newTree: IdBasedTree = {
        ...this.tree,
        expandedIds: new Set<bigint>(),
        collapsedIds: undefined,
      };
      this.tree = newTree;
      attrs.onTreeChanged?.(newTree);
    }
  }

  /**
   * Enables flat mode - shows only leaf-level rows without hierarchical grouping.
   */
  private enableFlatMode(attrs: DataGridAttrs): void {
    if (!this.pivot) return;
    const newPivot: Pivot = {...this.pivot, groupDisplay: 'flat'};
    this.pivot = newPivot;
    attrs.onPivotChanged?.(newPivot);
  }

  /**
   * Enables tree mode - shows hierarchical grouping with expand/collapse.
   */
  private enableTreeMode(attrs: DataGridAttrs): void {
    if (!this.pivot) return;
    const newPivot: Pivot = {...this.pivot, groupDisplay: 'tree'};
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
      aggregateSummaries,
      columnInfoCache,
      structuredQueryCompatMode,
      enablePivotControls,
      columnsAreMutable,
      filtersAreMutable,
    } = ctx;

    // Find the current sort direction (if any column is sorted)
    const currentSortDirection =
      this.columns.find((c) => c.sort)?.sort ?? 'ASC';

    const columns: GridColumn[] = this.columns.map((col, colIndex) => {
      const {id: colId, field, sort, aggregate} = col;
      const colAlias = getColumnAlias(col);

      // Get column info from cache
      const colInfo = columnInfoCache.get(field);

      // Build column title with chevron separators
      const titleParts = colInfo?.titleParts ?? field.split('.');
      const titleContent = buildColumnTitle(titleParts);

      // Build menu items
      const columnType = colInfo?.columnType;
      const menuItems: m.Children[] = [
        columnsAreMutable &&
          renderSortMenuItems(sort, (direction) =>
            this.updateSort(field, direction, attrs),
          ),
        columnsAreMutable && m(MenuDivider),
        filtersAreMutable &&
          m(FilterMenu, {
            datasource,
            field,
            columnType,
            structuredQueryCompatMode,
            valueFormatter: (v) => colInfo?.cellFormatter?.(v, {}) ?? String(v),
            onFilterAdd: (filter) => this.addFilter({field, ...filter}, attrs),
          }),
        filtersAreMutable && m(MenuDivider),
        this.gridApi &&
          m(MenuItem, {
            label: 'Fit to content',
            icon: 'fit_width',
            onclick: () => this.gridApi!.autoFitColumn(colId),
          }),
        columnsAreMutable && m(MenuDivider),
        columnsAreMutable &&
          m(ColumnMenu, {
            canAdd: attrs.canAddColumns ?? true,
            canRemove: this.columns.length > 1,
            onRemove:
              attrs.canRemoveColumns ?? true
                ? () => this.removeColumn(colId, attrs)
                : undefined,
            schema,
            rootSchema,
            visibleColumns: this.columns.map((c) => c.field),
            onAddColumn: (newField) =>
              this.addColumn(newField, attrs, colIndex),
            dataSource: datasource,
          }),
        attrs.addColumnMenuItems?.(field),
        m(MenuDivider),
        enablePivotControls &&
          columnsAreMutable &&
          m(MenuItem, {
            label: 'Group by this column',
            icon: 'pivot_table_chart',
            onclick: () => this.groupByColumn(field, attrs),
          }),
        columnsAreMutable && m(MenuDivider),
        // Summary menu - show available functions based on column type
        // Filter out ANY since it only makes sense in pivot mode (arbitrary value from group)
        columnsAreMutable &&
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
                  this.updateColumnAggregate(colId, undefined, attrs),
              }),
              ...funcs.map((func) =>
                m(MenuItem, {
                  label: func,
                  disabled: func === aggregate,
                  onclick: () => this.updateColumnAggregate(colId, func, attrs),
                }),
              ),
            ]);
          })(),
        m(MenuDivider),
        m(ColumnInfoMenu, {
          id: colId,
          field,
          colInfo,
          aggregateFunc: aggregate,
        }),
      ];

      // Build subContent showing grand total if column has an aggregate
      let subContent: m.Children;
      if (aggregate) {
        const totalValue = aggregateSummaries?.[colAlias];
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
        key: colId,
        header: m(
          GridHeaderCell,
          {
            sort: columnsAreMutable ? sort : undefined,
            hintSortDirection: currentSortDirection,
            onSort: columnsAreMutable
              ? (direction) => this.updateSort(colId, direction, attrs)
              : undefined,
            menuItems,
            subContent,
          },
          titleContent,
        ),
        reorderable: columnsAreMutable
          ? {reorderGroup: '__datagrid_columns__'}
          : undefined,
      };
    });

    return columns;
  }

  /**
   * Builds grid rows for flat (non-pivot) mode.
   */
  private buildFlatRows(ctx: FlatGridBuildContext): m.Children[][] {
    const {attrs, rowsResult, columnInfoCache, tree, filtersAreMutable} = ctx;

    if (rowsResult.rows === undefined) return [];

    // Find the intersection of rows between what we have and what is required
    // and only render those.
    const start = Math.max(rowsResult.rowOffset ?? 0, this.paginationOffset);

    const rowIndices = Array.from(
      {length: this.paginationLimit},
      (_, i) => i + start,
    );

    // ID-based tree mode config
    // Use specified treeColumn, or first visible column if not specified
    const treeColumn =
      tree?.treeColumn ?? (this.columns[0]?.field || undefined);

    return rowIndices
      .map((index) => {
        const row = rowsResult.rows![index - (rowsResult.rowOffset ?? 0)];
        if (row === undefined) return undefined;

        return this.columns.map((col) => {
          const {field} = col;
          const alias = getColumnAlias(col);
          const value = row[alias];
          const colInfo = columnInfoCache.get(field);
          const cellRenderer =
            colInfo?.cellRenderer ?? ((v: SqlValue) => renderCell(v, field));
          const rendered = cellRenderer(value, row);
          const isRich = isCellRenderResult(rendered);

          // Check if this is the tree column (id-based)
          const isTreeColumn = tree !== undefined && field === treeColumn;

          // Tree column specific rendering
          let chevron: 'expanded' | 'collapsed' | 'leaf' | undefined;
          let onChevronClick: (() => void) | undefined;
          let indent: number | undefined;

          if (isTreeColumn) {
            // ID-based tree mode
            const treeDepth = Number(row['__depth'] ?? 0);
            const hasChildren = Number(row['__has_children'] ?? 0) > 0;

            indent = treeDepth;

            if (hasChildren) {
              const nodeId = BigInt(row['__id'] as number | bigint);
              const isExpanded = this.isTreeNodeExpanded(nodeId);
              chevron = isExpanded ? 'expanded' : 'collapsed';
              onChevronClick = () => this.toggleTreeExpansion(nodeId, attrs);
            } else {
              chevron = 'leaf';
            }
          }

          return m(
            GridCell,
            {
              align: isRich ? rendered.align ?? 'left' : getAligment(value),
              nullish: isRich
                ? rendered.nullish ?? value === null
                : value === null,
              chevron,
              onChevronClick,
              indent,
              menuItems: filtersAreMutable
                ? [
                    m(CellFilterMenu, {
                      value,
                      onFilterAdd: (filter) =>
                        this.addFilter({field, ...filter}, attrs),
                    }),
                  ]
                : undefined,
            },
            isRich ? rendered.content : rendered,
          );
        });
      })
      .filter(exists);
  }

  /**
   * Checks if an id-based tree node is expanded.
   */
  private isTreeNodeExpanded(nodeId: bigint): boolean {
    if (!this.tree) return false;

    if (this.tree.collapsedIds) {
      // Denylist mode: expanded if NOT in collapsedIds
      return !this.tree.collapsedIds.has(nodeId);
    }

    if (this.tree.expandedIds) {
      // Allowlist mode: expanded only if in expandedIds
      return this.tree.expandedIds.has(nodeId);
    }

    // Default: collapsed
    return false;
  }

  /**
   * Toggles expansion of an id-based tree node.
   */
  private toggleTreeExpansion(nodeId: bigint, attrs: DataGridAttrs): void {
    if (!this.tree) return;

    const {idField, parentIdField, treeColumn} = this.tree;

    let newTree: IdBasedTree;

    if (this.tree.collapsedIds) {
      // Denylist mode: toggle in collapsedIds
      const newCollapsed = new Set<bigint>(this.tree.collapsedIds);
      if (newCollapsed.has(nodeId)) {
        newCollapsed.delete(nodeId);
      } else {
        newCollapsed.add(nodeId);
      }
      newTree = {
        idField,
        parentIdField,
        treeColumn,
        collapsedIds: newCollapsed,
      };
    } else {
      // Allowlist mode: toggle in expandedIds
      const currentExpanded = this.tree.expandedIds ?? new Set<bigint>();
      const newExpanded = new Set<bigint>(currentExpanded);
      if (newExpanded.has(nodeId)) {
        newExpanded.delete(nodeId);
      } else {
        newExpanded.add(nodeId);
      }
      newTree = {
        idField,
        parentIdField,
        treeColumn,
        expandedIds: newExpanded,
      };
    }

    this.tree = newTree;
    attrs.onTreeChanged?.(newTree);
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
      aggregateSummaries,
      pivot,
      structuredQueryCompatMode,
      enablePivotControls,
      filtersAreMutable,
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
      const colId = groupByCol.id;
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
        filtersAreMutable && m(MenuDivider),
        filtersAreMutable &&
          m(FilterMenu, {
            datasource,
            field,
            columnType,
            structuredQueryCompatMode,
            valueFormatter: (v) => colInfo?.cellFormatter?.(v, {}) ?? String(v),
            onFilterAdd: (filter) => this.addFilter({field, ...filter}, attrs),
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
            canRemove: true,
            onRemove: () => this.removeGroupByColumn(i, attrs),
            removeLabel: 'Remove group by',
            addLabel: 'Add group by',
          }),
        m(MenuDivider),
        m(ColumnInfoMenu, {id: colId, field, colInfo}),
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
      if (agg.function === 'COUNT') {
        title = 'Count';
      } else {
        colInfo = getColumnInfo(schema, rootSchema, agg.field);
        const fieldTitle = colInfo?.titleParts ?? [agg.field];
        title = buildColumnTitle(fieldTitle);
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
          id: agg.id,
          field: 'field' in agg ? agg.field : alias,
          colInfo,
          aggregateFunc: agg.function,
        }),
      ];

      // Build subContent showing grand total with aggregate symbol
      // Don't show grand total for ANY aggregation (it's just an arbitrary value)
      const aggregateTotalValue = aggregateSummaries?.[alias];
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
   * For multi-level pivots with rollups, adds expand/collapse chevrons.
   */
  private buildPivotRows(ctx: PivotGridBuildContext): m.Children[][] {
    const {
      attrs,
      schema,
      rootSchema,
      rowsResult,
      pivot,
      enablePivotControls,
      filtersAreMutable,
    } = ctx;

    if (rowsResult.rows === undefined) return [];

    // Find the intersection of rows between what we have and what is required
    // and only render those.
    const start = Math.max(rowsResult.rowOffset ?? 0, this.paginationOffset);

    const rowIndices = Array.from(
      {length: this.paginationLimit},
      (_, i) => i + start,
    );

    const aggregates = pivot.aggregates ?? [];
    const numGroupBy = pivot.groupBy.length;
    // In flat mode, don't use multi-level UI (no chevrons, no indent)
    const isMultiLevel = numGroupBy > 1 && pivot.groupDisplay === 'tree';

    return rowIndices
      .map((index) => {
        const row = rowsResult.rows![index - (rowsResult.rowOffset ?? 0)];
        if (row === undefined) return undefined;

        const cells: m.Children[] = [];

        // For multi-level pivots, get the rollup level from __depth column
        // __depth is 0 for root, 1 for first groupBy level, etc.
        // Subtract 1 to get 0-indexed column position for chevron placement.
        const rowLevel = isMultiLevel
          ? Number(row['__depth'] ?? numGroupBy) - 1
          : numGroupBy - 1;

        // Render groupBy columns
        for (let i = 0; i < numGroupBy; i++) {
          const groupByCol = pivot.groupBy[i];
          const {id, field} = groupByCol;
          const value = row[id];

          // Column is a rollup if its index is greater than the row's level
          const isRollupColumn = i > rowLevel;

          // For rollup columns, show empty content
          if (isRollupColumn) {
            cells.push(
              m(GridCell, {className: 'pf-data-grid__groupby-column'}),
            );
            continue;
          }

          // Get cell renderer from schema
          const colInfo = getColumnInfo(schema, rootSchema, field);
          const cellRenderer =
            colInfo?.cellRenderer ?? ((v: SqlValue) => renderCell(v, field));
          const rendered = cellRenderer(value, row);
          const isRich = isCellRenderResult(rendered);

          // Determine chevron state for groupBy columns on summary rows
          let chevron: 'expanded' | 'collapsed' | 'leaf' | undefined;
          let onChevronClick: (() => void) | undefined;
          let indent: number | undefined;
          let className: string | undefined;

          if (isMultiLevel) {
            // This is a summary row if rowLevel equals this column's index
            // (meaning next column onwards are rollups)
            const isSummaryRow = rowLevel === i;

            if (isSummaryRow && i < numGroupBy - 1) {
              // Build the group path from the row's group columns
              const groupPath: SqlValue[] = [];
              for (let g = 0; g <= i; g++) {
                groupPath.push(row[`__group_${g}`] as SqlValue);
              }
              // This is a summary row at this level - show expand/collapse chevron
              const isExpanded = this.isGroupExpanded(groupPath);
              chevron = isExpanded ? 'expanded' : 'collapsed';
              onChevronClick = () => this.toggleExpansion(groupPath, attrs);
            } else if (i < rowLevel) {
              // This column has a value but is not the summary level - it's a leaf cell
              // in a higher-level column, show with indent
              chevron = 'leaf';
              className = 'pf-data-grid__groupby-muted';
            }
            // If i === numGroupBy - 1 and rowLevel === numGroupBy - 1, it's a leaf row
            // with no chevron needed
          }

          cells.push(
            m(
              GridCell,
              {
                align: isRich ? rendered.align ?? 'left' : getAligment(value),
                nullish: isRich
                  ? rendered.nullish ?? value === null
                  : value === null,
                className: classNames(
                  'pf-data-grid__groupby-column',
                  className,
                ),
                chevron,
                onChevronClick,
                indent,
                menuItems: [
                  enablePivotControls && [
                    m(MenuItem, {
                      label: 'Drill down',
                      icon: 'zoom_in',
                      onclick: () => {
                        // Build a row with only the groupBy columns up to rowLevel
                        const drillDownRow: {field: string; value: SqlValue}[] =
                          [];
                        for (let j = 0; j <= rowLevel; j++) {
                          const col = pivot.groupBy[j];
                          drillDownRow.push({
                            field: col.field,
                            value: row[col.id],
                          });
                        }
                        this.drillDown(drillDownRow, attrs);
                      },
                    }),
                    filtersAreMutable && m(MenuDivider),
                  ],
                  filtersAreMutable &&
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
                // Build a row with only the groupBy columns up to rowLevel
                const drillDownRow: {field: string; value: SqlValue}[] = [];
                for (let j = 0; j <= rowLevel; j++) {
                  const col = pivot.groupBy[j];
                  drillDownRow.push({field: col.field, value: row[col.id]});
                }
                this.drillDown(drillDownRow, attrs);
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
          let cellRenderer =
            'field' in agg
              ? getColumnInfo(schema, rootSchema, agg.field)?.cellRenderer
              : undefined;
          cellRenderer ??= (v: SqlValue) => renderCell(v, alias);

          const rendered = cellRenderer(value, row);
          const isRich = isCellRenderResult(rendered);

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
              isRich ? rendered.content : rendered,
            ),
          );
        }

        return cells;
      })
      .filter(exists);
  }

  private async formatData(
    dataSource: DataSource,
    model: DataSourceModel,
    schema: SchemaRegistry | undefined,
    rootSchema: string | undefined,
    pivot: Pivot | undefined,
    format: 'tsv' | 'json' | 'markdown' = 'tsv',
  ): Promise<string> {
    // Get all rows from the data source
    const rows = await dataSource.exportData(model);

    // Determine which columns to export based on mode
    let columns: ReadonlyArray<string>;
    let columnNames: Record<string, string>;

    // Check if we're in pivot mode (has groupBy columns and not drilling down)
    const isPivotMode =
      pivot !== undefined &&
      pivot.groupBy.length > 0 &&
      pivot.drillDown === undefined;

    if (isPivotMode) {
      // In pivot mode, export groupBy IDs + aggregate IDs (which are SQL aliases)
      columns = [
        ...pivot.groupBy.map(({id}) => id),
        ...(pivot.aggregates ?? []).map((agg) => getAggregateAlias(agg)),
      ];

      // Build custom column names for pivot mode
      columnNames = {};

      // Add groupBy column names (keyed by ID)
      for (const groupByCol of pivot.groupBy) {
        const {id, field} = groupByCol;
        const colInfo =
          schema && rootSchema
            ? getColumnInfo(schema, rootSchema, field)
            : undefined;
        columnNames[id] = colInfo?.def.titleString ?? field;
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
      // Use aliases (column IDs) for accessing row data
      columns = this.columns.map((c) => getColumnAlias(c));

      // Build column names using field paths for schema lookup
      const fieldPaths = this.columns.map((c) => c.field);
      columnNames = buildColumnNames(schema, rootSchema, fieldPaths);

      // Map aliases to field paths for column name lookup
      for (let i = 0; i < columns.length; i++) {
        const alias = columns[i];
        const field = fieldPaths[i];
        if (alias !== field) {
          columnNames[alias] = columnNames[field];
        }
      }
    }

    // Build alias-to-field mapping for formatRows
    const aliasToField: Record<string, string> = {};
    for (const col of this.columns) {
      aliasToField[getColumnAlias(col)] = col.field;
    }

    const formattedRows = formatRows(
      rows,
      schema,
      rootSchema,
      columns,
      aliasToField,
    );

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
function buildColumnTitle(titleParts: m.ChildArray): m.Children[] {
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
 * Uses the column's unique ID as the alias.
 */
function getAggregateAlias(agg: AggregateColumn): string {
  return agg.id;
}

/**
 * Get the alias used for a column in query results.
 * Uses the column's unique ID as the alias.
 */
function getColumnAlias(col: Column): string {
  return col.id;
}
