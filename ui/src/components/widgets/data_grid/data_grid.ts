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
import {classNames} from '../../../base/classnames';
import {download} from '../../../base/download_utils';
import {Icons} from '../../../base/semantic_icons';
import {exists} from '../../../base/utils';
import {SqlValue} from '../../../trace_processor/query_result';
import {Anchor} from '../../../widgets/anchor';
import {Button, ButtonVariant} from '../../../widgets/button';
import {EmptyState} from '../../../widgets/empty_state';
import {LinearProgress} from '../../../widgets/linear_progress';
import {MenuDivider, MenuItem} from '../../../widgets/menu';
import {
  renderSortMenuItems,
  Grid,
  GridApi,
  GridColumn,
  GridCell,
  GridHeaderCell,
} from '../../../widgets/grid';
import {
  AggregationFunction,
  BuiltinMenuItems,
  ColumnDefinition,
  DataGridDataSource,
  DataGridFilter,
  DEFAULT_SUPPORTED_FILTERS,
  FilterType,
  isNumeric,
  PivotModel,
  PivotValue,
  RowDef,
  Sorting,
} from './common';
import {InMemoryDataSource} from './in_memory_data_source';
import {
  defaultValueFormatter,
  formatAsTSV,
  formatAsJSON,
  formatAsMarkdown,
} from './export_utils';
import {DataGridToolbar} from './data_grid_toolbar';
import {SortDirection} from '../../../base/comparison_utils';
import {AggregationCell} from './aggregation_cell';
import {renderPivotMenu, renderAggregateActions} from './pivot_menu';
import {renderFilterSubmenuItems} from './filter_menu';
import {renderColumnManagementMenu} from './column_menu';
import {buildCellContextMenu} from './cell_filter_menu';

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

type OnFilterAdd = (filter: DataGridFilter) => void;
export type OnFilterRemove = (index: number) => void;
type OnSortingChanged = (sorting: Sorting) => void;
type OnPivotChanged = (pivot: PivotModel | undefined) => void;
type ColumnOrder = ReadonlyArray<string>;
type OnColumnOrderChanged = (columnOrder: ColumnOrder) => void;

function noOp() {}

export interface DataGridAttrs {
  /**
   * Defines the columns to be displayed in the data grid and how they are
   * displayed.
   */
  readonly columns: ReadonlyArray<ColumnDefinition>;

  /**
   * The data source that provides rows to the grid. Responsible for fetching,
   * filtering, and sorting data based on the current state.
   *
   * The data source is responsible for applying the filters, sorting, and
   * paging and providing the rows that are displayed in the grid.
   */
  readonly data: DataGridDataSource | ReadonlyArray<RowDef>;

  /**
   * Current sort configuration - can operate in controlled or uncontrolled
   * mode.
   *
   * In controlled mode: Provide this prop along with onSortingChanged callback.
   * In uncontrolled mode: Omit this prop to let the grid manage sorting state
   * internally.
   *
   * Specifies which column to sort by and the direction (asc/DESC/unsorted). If
   * not provided, defaults to internal state with direction 'unsorted'.
   */
  readonly sorting?: Sorting;

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
  readonly pivot?: PivotModel;

  /**
   * Initial pivot configuration to apply on first load.
   * This is ignored in controlled mode (i.e. when `pivot` is provided).
   */
  readonly initialPivot?: PivotModel;

  /**
   * Callback triggered when the pivot configuration changes.
   * Allows parent components to react to pivot changes.
   * Required for controlled mode - when provided with pivot,
   * the parent component becomes responsible for updating the pivot prop.
   * @param pivot The new pivot configuration (or undefined to disable pivoting)
   */
  readonly onPivotChanged?: OnPivotChanged;

  /**
   * Initial sorting to apply to the grid on first load.
   * This is ignored in controlled mode (i.e. when `sorting` is provided).
   */
  readonly initialSorting?: Sorting;

  /**
   * Callback triggered when the sort configuration changes.
   * Allows parent components to react to sorting changes.
   * Required for controlled mode sorting - when provided with sorting,
   * the parent component becomes responsible for updating the sorting prop.
   * @param sorting The new sort configuration
   */
  readonly onSort?: OnSortingChanged;

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
  readonly filters?: ReadonlyArray<DataGridFilter>;

  /**
   * Initial filters to apply to the grid on first load.
   * This is ignored in controlled mode (i.e. when `filters` is provided).
   */
  readonly initialFilters?: ReadonlyArray<DataGridFilter>;

  /**
   * These callbacks are triggered when filters are added or removed by the
   * user. They are only called in controlled mode, e.g. only if filters is
   * provided.
   */
  readonly onFilterAdd?: OnFilterAdd;
  readonly onFilterRemove?: OnFilterRemove;
  readonly clearFilters?: () => void;

  /**
   * Order of columns to display - can operate in controlled or uncontrolled
   * mode.
   *
   * In controlled mode: Provide this prop along with onColumnOrderChanged callback.
   * In uncontrolled mode: Omit this prop to let the grid manage order internally.
   *
   * Array of column names in the order they should be displayed.
   * If not provided, columns are displayed in the order given in the columns prop.
   */
  readonly columnOrder?: ColumnOrder;

  /**
   * Initial column order to apply on first load.
   * This is ignored in controlled mode (i.e. when `columnOrder` is provided).
   */
  readonly initialColumnOrder?: ColumnOrder;

  /**
   * Callback triggered when columns are reordered via drag-and-drop.
   * Allows parent components to react to reordering changes.
   * Required for controlled mode - when provided with columnOrder,
   * the parent component becomes responsible for updating the columnOrder prop.
   * @param columnOrder The new array of column names in display order
   */
  readonly onColumnOrderChanged?: OnColumnOrderChanged;

  /**
   * Whether to enable column reordering via drag-and-drop.
   * Default = true if onColumnOrderChanged is provided, false otherwise.
   */
  readonly columnReordering?: boolean;

  /**
   * Display applied filters in the toolbar. Set to false to hide them, for
   * example, if filters are displayed elsewhere in the UI. This does not
   * disable filtering functionality.
   *
   * Defaults to true.
   */
  readonly showFiltersInToolbar?: boolean;

  /**
   * Fill parent container vertically.
   */
  readonly fillHeight?: boolean;

  /**
   * Extra items to place on the toolbar.
   */
  readonly toolbarItemsLeft?: m.Children;

  /**
   * Extra items to place on the toolbar.
   */
  readonly toolbarItemsRight?: m.Children;

  /**
   * Optional class name added to the root element of the data grid.
   */
  readonly className?: string;

  /**
   * Enable export buttons in toolbar. When enabled, adds Copy and Download
   * buttons that export the current filtered/sorted data.
   * Default = false.
   */
  readonly showExportButton?: boolean;

  /**
   * Show row count in toolbar. Displays the number of filtered rows and total rows.
   * Default = false.
   */
  readonly showRowCount?: boolean;

  /**
   * Callback that receives the DataGrid API when the grid is ready.
   * Allows parent components to programmatically export data.
   */
  readonly onReady?: (api: DataGridApi) => void;

  /**
   * Specify which filter types should be available in the column filter menu.
   * Default includes all filter types: ['null', 'equals', 'contains', 'glob']
   *
   * Available filter types:
   * - 'null': Filter out nulls / Only show nulls
   * - 'equals': Equals to... / Not equals to... (uses distinct values)
   * - 'contains': Contains... / Not contains...
   * - 'glob': Glob... / Not glob...
   */
  readonly supportedFilters?: ReadonlyArray<FilterType>;
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
  getRowCount(): number;
}

export class DataGrid implements m.ClassComponent<DataGridAttrs> {
  // Internal state
  private sorting: Sorting = {direction: 'UNSORTED'};
  private filters: ReadonlyArray<DataGridFilter> = [];
  private pivot: PivotModel | undefined = undefined;
  private columnOrder: ColumnOrder = [];
  // Track all columns we've ever seen to distinguish hidden vs new columns
  private seenColumns: Set<string> = new Set();
  // Track pagination state from virtual scrolling
  private paginationOffset: number = 0;
  private paginationLimit: number = 100;
  private gridApi?: GridApi;
  // Track columns needing distinct values
  private distinctValuesColumns = new Set<string>();
  private dataGridApi: DataGridApi = {
    exportData: async (format) => {
      if (!this.currentDataSource || !this.currentColumns) {
        throw new Error('DataGrid not ready for export');
      }
      return await this.formatData(
        this.currentDataSource,
        this.currentColumns,
        format,
      );
    },
    getRowCount: () => {
      return this.currentDataSource?.rows?.totalRows ?? 0;
    },
  };
  private currentDataSource?: DataGridDataSource;
  private currentColumns?: ReadonlyArray<ColumnDefinition>;

  oninit({attrs}: m.Vnode<DataGridAttrs>) {
    if (attrs.initialSorting) {
      this.sorting = attrs.initialSorting;
    }

    if (attrs.initialFilters) {
      this.filters = attrs.initialFilters;
    }

    if (attrs.initialPivot) {
      this.pivot = attrs.initialPivot;
    }

    // Initialize column order from initial prop or columns array
    if (attrs.initialColumnOrder) {
      this.columnOrder = attrs.initialColumnOrder;
    } else {
      this.columnOrder = attrs.columns.map((c) => c.name);
    }

    // Track all initial columns as seen
    attrs.columns.forEach((column) => {
      this.seenColumns.add(column.name);
    });
  }

  view({attrs}: m.Vnode<DataGridAttrs>) {
    const {
      columns,
      data,
      sorting = this.sorting,
      onSort = sorting === this.sorting ? (x) => (this.sorting = x) : noOp,
      filters = this.filters,
      onFilterAdd = filters === this.filters
        ? (filter) => {
            this.filters = [...this.filters, filter];
          }
        : noOp,
      onFilterRemove = filters === this.filters
        ? (index) => {
            const newFilters = this.filters.filter((_, i) => i !== index);
            this.filters = newFilters;
          }
        : noOp,
      clearFilters = filters === this.filters
        ? () => {
            this.filters = [];
          }
        : noOp,
      pivot = this.pivot,

      onPivotChanged = pivot === this.pivot
        ? (x: PivotModel | undefined) => (this.pivot = x)
        : noOp,
      columnOrder = this.columnOrder,
      onColumnOrderChanged = columnOrder === this.columnOrder
        ? (x) => (this.columnOrder = x)
        : noOp,
      columnReordering = onColumnOrderChanged !== noOp ||
        onPivotChanged !== noOp,
      showFiltersInToolbar = true,
      fillHeight = false,
      toolbarItemsLeft,
      toolbarItemsRight,
      className,
      showExportButton = false,
      showRowCount = false,
      onReady,
      supportedFilters = DEFAULT_SUPPORTED_FILTERS,
    } = attrs;

    // In uncontrolled mode, sync columnOrder with truly new columns
    // (not hidden columns)
    if (columnOrder === this.columnOrder) {
      const newColumns = columns
        .map((c) => c.name)
        .filter((name) => !this.seenColumns.has(name));

      if (newColumns.length > 0) {
        // Add newly seen columns to tracking and order
        newColumns.forEach((name) => this.seenColumns.add(name));
        this.columnOrder = [...this.columnOrder, ...newColumns];
      }
    }

    let orderedColumns: ReadonlyArray<ColumnDefinition>;
    const isDrillDown = pivot?.drillDown !== undefined;
    if (pivot && !isDrillDown) {
      // Pivot mode (not drill-down): show pivot columns
      const columnMap = new Map(columns.map((c) => [c.name, c]));
      const pivotColumns: ColumnDefinition[] = [];

      // Add groupBy columns
      for (const colName of pivot.groupBy) {
        const colDef = columnMap.get(colName);
        if (colDef) {
          pivotColumns.push(colDef);
        }
      }

      // Add value columns
      for (const [alias, value] of Object.entries(pivot.values)) {
        if (value.func === 'COUNT') {
          // Handle COUNT which doesn't have a source column
          const newColDef: ColumnDefinition = {
            name: alias,
            title: 'Count',
            aggregation: value.func,
          };
          pivotColumns.push(newColDef);
        } else {
          if (value.col) {
            const sourceColDef = columnMap.get(value.col);
            if (sourceColDef) {
              // Create a new, synthetic column definition for the pivoted value
              const newColDef: ColumnDefinition = {
                ...sourceColDef, // Inherit properties from the source column
                name: alias, // The name (key) of this column is the alias
                title: sourceColDef.title ?? sourceColDef.name,
                aggregation: value.func,
              };
              pivotColumns.push(newColDef);
            }
          }
        }
      }
      orderedColumns = pivotColumns;
    } else if (isDrillDown) {
      // Drill-down mode: show ALL original columns (ignore column hiding)
      orderedColumns = columns;
    } else {
      // Normal mode: show columns according to columnOrder (respects hiding)
      orderedColumns = this.getOrderedColumns(columns, columnOrder);
    }

    // Initialize the datasource if required
    let dataSource: DataGridDataSource;
    if (Array.isArray(data)) {
      // If raw data supplied - just create a new in memory data source every
      // render cycle.
      dataSource = new InMemoryDataSource(data);
    } else {
      dataSource = data as DataGridDataSource;
    }

    // Update datasource with current state (sorting, filtering, pagination)
    // This is called every view cycle to catch changes
    dataSource.notifyUpdate({
      columns: orderedColumns.map((c) => c.name),
      sorting,
      filters,
      pagination: {
        offset: this.paginationOffset,
        limit: this.paginationLimit,
      },
      aggregates: orderedColumns
        .filter((c) => c.aggregation)
        .map((c) => ({col: c.name, func: c.aggregation!})),
      pivot,
      distinctValuesColumns: this.distinctValuesColumns,
    });

    // Store current state for API access
    this.currentDataSource = dataSource;
    this.currentColumns = orderedColumns;

    // Create and expose DataGrid API if needed
    onReady?.(this.dataGridApi);

    const sortControls = onSort !== noOp;
    const filtersUncontrolled = filters === this.filters;
    const filterControls = Boolean(
      filtersUncontrolled || onFilterAdd !== noOp || onFilterRemove !== noOp,
    );

    const lastGroupByColName =
      pivot && pivot.groupBy.length > 0
        ? pivot.groupBy[pivot.groupBy.length - 1]
        : undefined;

    // Build VirtualGrid columns with all DataGrid features
    const virtualGridColumns = orderedColumns.map((column) => {
      const sortDirection = (() => {
        if (sorting.direction === 'UNSORTED') {
          return undefined;
        } else if (sorting.column === column.name) {
          return sorting.direction;
        } else {
          return undefined;
        }
      })();

      const menuItems = this.renderColumnContextMenuItems(
        column,
        columns,
        sortDirection,
        dataSource,
        onSort,
        onFilterAdd,
        supportedFilters,
        sortControls,
        filterControls,
        columnOrder,
        orderedColumns,
        onColumnOrderChanged,
        pivot,
        onPivotChanged,
      );

      // Build aggregation sub-content if needed (but not for ANY which is just a passthrough)
      const subContent =
        column.aggregation && column.aggregation !== 'ANY'
          ? m(
              AggregationCell,
              {
                symbol: aggregationFunctionSymbol(column.aggregation),
              },
              column.cellRenderer &&
                exists(dataSource.rows?.aggregates?.[column.name])
                ? column.cellRenderer(
                    dataSource.rows?.aggregates[column.name],
                    dataSource.rows.aggregates,
                  )
                : renderCell(
                    dataSource.rows?.aggregates?.[column.name],
                    column.name,
                  ),
            )
          : undefined;

      const isLastGroupBy = lastGroupByColName === column.name;
      const isGroupByColumn = pivot?.groupBy.includes(column.name) ?? false;
      const isAggregateColumn = pivot?.values?.[column.name] !== undefined;

      // Determine the reorder group:
      // - In pivot mode: separate groups for groupBy and aggregate columns
      // - In normal mode: single group for all columns
      const reorderGroup = (() => {
        if (!columnReordering) return undefined;
        if (pivot) {
          if (isGroupByColumn) return 'datagrid-pivot-groupby';
          if (isAggregateColumn) return 'datagrid-pivot-aggregates';
          return undefined; // Shouldn't happen in pivot mode
        }
        return 'datagrid-columns';
      })();

      const gridColumn: GridColumn = {
        key: column.name,
        header: m(
          GridHeaderCell,
          {
            sort: sortDirection,
            hintSortDirection:
              sorting.direction === 'UNSORTED' ? undefined : sorting.direction,
            onSort: sortControls
              ? (direction) => {
                  onSort({
                    column: column.name,
                    direction,
                  });
                }
              : undefined,
            menuItems:
              menuItems !== undefined &&
              Array.isArray(menuItems) &&
              menuItems.length > 0
                ? menuItems
                : menuItems !== undefined
                  ? menuItems
                  : undefined,
            subContent,
          },
          column.title ?? column.name,
        ),
        thickRightBorder: isLastGroupBy && !isDrillDown,
        reorderable: reorderGroup ? {handle: reorderGroup} : undefined,
      };

      return gridColumn;
    });

    // Add drill-down column when in pivot mode (not drill-down mode)
    const pivotControls = onPivotChanged !== noOp;
    const showDrillDownColumn = pivot && !isDrillDown && pivotControls;
    if (showDrillDownColumn) {
      virtualGridColumns.push({
        key: '__drilldown__',
        header: m(GridHeaderCell, ''),
        // width: 40,
      });
    }

    const rows = dataSource.rows;
    const virtualGridRows = (() => {
      if (!rows) return [];

      // Find the intersection of rows between what we have and what is required
      // and only render those.

      const start = Math.max(rows.rowOffset, this.paginationOffset);

      const rowIndices = Array.from(
        {length: this.paginationLimit},
        (_, i) => i + start,
      );

      // Convert RowDef data to vnode rows for VirtualGrid
      return rowIndices
        .map((index) => {
          const row = rows.rows[index - rows.rowOffset];
          if (row === undefined) return undefined;
          const cellRow: m.Children[] = [];

          orderedColumns.forEach((column) => {
            const value = row[column.name];
            const menuItems = buildCellContextMenu(
              column,
              value,
              row,
              supportedFilters,
              onFilterAdd,
              filterControls,
            );

            // Build cell - use GridDataCell when we have menus or special rendering
            cellRow.push(
              m(
                GridCell,
                {
                  align: isNumeric(value)
                    ? 'right'
                    : value === null
                      ? 'center'
                      : 'left',
                  nullish: value === null,
                  menuItems: menuItems.length > 0 ? menuItems : undefined,
                },
                column.cellRenderer
                  ? column.cellRenderer(value, row)
                  : renderCell(value, column.name),
              ),
            );
          });

          // Add drill-down button cell when in pivot mode
          if (showDrillDownColumn) {
            // Build the drillDown values from the groupBy columns
            const drillDownValues: RowDef = {};
            for (const colName of pivot.groupBy) {
              drillDownValues[colName] = row[colName];
            }
            cellRow.push(
              m(Button, {
                icon: Icons.GoTo,
                title: 'Drill down into this group',
                onclick: () => {
                  onPivotChanged({
                    ...pivot,
                    drillDown: drillDownValues,
                  });
                },
              }),
            );
          }

          return cellRow;
        })
        .filter(exists);
    })();

    return m(
      '.pf-data-grid',
      {
        className: classNames(
          fillHeight && 'pf-data-grid--fill-height',
          className,
        ),
      },
      m(DataGridToolbar, {
        filters,
        columns,
        totalRows: rows?.totalRows ?? 0,
        showFilters: showFiltersInToolbar,
        showRowCount,
        showExportButton,
        toolbarItemsLeft,
        toolbarItemsRight,
        dataGridApi: this.dataGridApi,
        onFilterRemove,
        formatFilter: this.formatFilter.bind(this),
        pivot:
          pivot && pivotControls
            ? {
                groupByColumns: pivot.groupBy,
                onExit: () => {
                  // Exit pivot mode entirely
                  onPivotChanged(undefined);
                },
              }
            : undefined,
        drillDown:
          pivot?.drillDown && pivotControls
            ? {
                drillDown: pivot.drillDown,
                groupByColumns: pivot.groupBy,
                onBack: () => {
                  // Remove drillDown from pivot to return to pivot view
                  const {drillDown: _, ...pivotWithoutDrillDown} = pivot;
                  onPivotChanged(pivotWithoutDrillDown as typeof pivot);
                },
              }
            : undefined,
      }),
      m(LinearProgress, {
        className: 'pf-data-grid__loading',
        state: dataSource.isLoading ? 'indeterminate' : 'none',
      }),
      m(Grid, {
        className: 'pf-data-grid__table',
        columns: virtualGridColumns,
        rowData: {
          data: virtualGridRows,
          total: rows?.totalRows ?? 0,
          offset: Math.max(rows?.rowOffset ?? 0, this.paginationOffset),
          onLoadData: (offset, limit) => {
            // Store pagination state and trigger redraw
            this.paginationOffset = offset;
            this.paginationLimit = limit;
            m.redraw();
          },
        },
        virtualization: {
          rowHeightPx: 25,
        },
        fillHeight: true,
        onColumnReorder: columnReordering
          ? (from, to, position) => {
              if (pivot) {
                // In pivot mode, reorder within the pivot model
                const newPivot = this.reorderPivotColumns(
                  pivot,
                  from,
                  to,
                  position,
                );
                onPivotChanged(newPivot);
              } else {
                const newOrder = this.reorderColumns(
                  columnOrder,
                  from,
                  to,
                  position,
                );
                onColumnOrderChanged(newOrder);
              }
            }
          : undefined,
        onReady: (api) => {
          this.gridApi = api;
        },
        emptyState:
          rows?.totalRows === 0 && !dataSource.isLoading
            ? m(
                EmptyState,
                {
                  title:
                    filters.length > 0
                      ? 'No results match your filters'
                      : 'No data available',
                  fillHeight: true,
                },
                filters.length > 0 &&
                  m(Button, {
                    variant: ButtonVariant.Filled,
                    icon: Icons.FilterOff,
                    label: 'Clear filters',
                    onclick: clearFilters,
                  }),
              )
            : undefined,
      }),
    );
  }

  private renderColumnContextMenuItems(
    column: ColumnDefinition,
    columns: ReadonlyArray<ColumnDefinition>,
    sort: SortDirection | undefined,
    dataSource: DataGridDataSource,
    onSort: OnSortingChanged,
    onFilterAdd: OnFilterAdd,
    supportedFilters: ReadonlyArray<FilterType>,
    sortControls: boolean,
    filterControls: boolean,
    columnOrder: ColumnOrder,
    orderedColumns: ReadonlyArray<ColumnDefinition>,
    onColumnOrderChanged: OnColumnOrderChanged,
    pivot: PivotModel | undefined,
    onPivotChanged: OnPivotChanged,
  ): m.Children | undefined {
    const pivotControls = onPivotChanged !== noOp;
    const isCurrentColumnAggregate = pivot?.values?.[column.name] !== undefined;
    const currentGroupBy = pivot?.groupBy ?? [];
    const isCurrentColumnGrouped = currentGroupBy.includes(column.name);

    const builtins: Partial<BuiltinMenuItems> = {
      sorting: sortControls && [
        ...renderSortMenuItems(sort, (direction) => {
          if (direction) {
            onSort({
              column: column.name,
              direction: direction,
            });
          } else {
            onSort({
              direction: 'UNSORTED',
            });
          }
        }),
      ],
      // Column management is only available when not in pivot mode
      // In pivot mode, column visibility is controlled by the pivot state
      columnManagement: !pivot && [
        orderedColumns.length > 1 &&
          m(MenuItem, {
            label: 'Hide column',
            icon: Icons.Hide,
            onclick: () => {
              const newOrder = columnOrder.filter(
                (name) => name !== column.name,
              );
              onColumnOrderChanged(newOrder);
            },
          }),
        renderColumnManagementMenu(columns, columnOrder, onColumnOrderChanged),
      ],
      fitToContent: m(MenuItem, {
        label: 'Fit to content',
        icon: 'fit_width',
        onclick: () => this.gridApi?.autoFitColumn(column.name),
      }),
      filters: filterControls && [
        m(
          MenuItem,
          {label: 'Add filter...', icon: Icons.Filter},
          renderFilterSubmenuItems(
            column,
            onFilterAdd,
            dataSource.rows?.distinctValues?.get(column.name),
            supportedFilters,
            this.distinctValuesColumns,
          ),
        ),
      ],
      pivot: pivotControls && [
        renderPivotMenu(columns, pivot, onPivotChanged, column),
      ],
    };

    // Build final menu items using contextMenuRenderer if provided
    const menuItems: m.Children = column.contextMenuRenderer
      ? column.contextMenuRenderer(builtins)
      : [
          builtins.sorting,
          m(MenuDivider),
          builtins.filters,
          m(MenuDivider),
          // All pivot-related items grouped together
          // If current column is an aggregate, show remove/change at top level
          isCurrentColumnAggregate &&
            renderAggregateActions(
              column,
              pivot!,
              onPivotChanged,
              currentGroupBy,
              columns,
            ),
          // If current column is grouped, show remove pivot at top level
          isCurrentColumnGrouped &&
            m(MenuItem, {
              label: 'Remove',
              icon: Icons.Delete,
              onclick: () => {
                const newGroupBy = currentGroupBy.filter(
                  (name) => name !== column.name,
                );
                if (
                  newGroupBy.length === 0 &&
                  Object.keys(pivot?.values ?? {}).length === 0
                ) {
                  // If no groupBy and no values, clear the pivot entirely
                  onPivotChanged(undefined);
                } else {
                  const newPivot: PivotModel = {
                    groupBy: newGroupBy,
                    values: pivot?.values ?? {},
                  };
                  onPivotChanged(newPivot);
                }
              },
            }),
          builtins.pivot,
          m(MenuDivider),
          builtins.fitToContent,
          m(MenuDivider),
          builtins.columnManagement,
        ];

    return menuItems;
  }

  private async formatData(
    dataSource: DataGridDataSource,
    columns: ReadonlyArray<ColumnDefinition>,
    format: 'tsv' | 'json' | 'markdown' = 'tsv',
  ): Promise<string> {
    // Get all rows from the data source
    const rows = await dataSource.exportData();

    // Format the data based on the requested format
    switch (format) {
      case 'tsv':
        return this.formatAsTSV(rows, columns);
      case 'json':
        return this.formatAsJSON(rows, columns);
      case 'markdown':
        return this.formatAsMarkdown(rows, columns);
    }
  }

  private formatAsTSV(
    rows: readonly RowDef[],
    columns: ReadonlyArray<ColumnDefinition>,
  ): string {
    const formattedRows = this.formatRows(rows, columns);
    const columnNames = this.buildColumnNames(columns);
    return formatAsTSV(
      columns.map((c) => c.name),
      columnNames,
      formattedRows,
    );
  }

  private formatAsJSON(
    rows: readonly RowDef[],
    columns: ReadonlyArray<ColumnDefinition>,
  ): string {
    const formattedRows = this.formatRows(rows, columns);
    return formatAsJSON(formattedRows);
  }

  private formatAsMarkdown(
    rows: readonly RowDef[],
    columns: ReadonlyArray<ColumnDefinition>,
  ): string {
    const formattedRows = this.formatRows(rows, columns);
    const columnNames = this.buildColumnNames(columns);
    return formatAsMarkdown(
      columns.map((c) => c.name),
      columnNames,
      formattedRows,
    );
  }

  private formatRows(
    rows: readonly RowDef[],
    columns: ReadonlyArray<ColumnDefinition>,
  ): Array<Record<string, string>> {
    return rows.map((row) => {
      const formattedRow: Record<string, string> = {};
      for (const col of columns) {
        const value = row[col.name];
        const formatter = col.valueFormatter ?? defaultValueFormatter;
        formattedRow[col.name] = formatter(value, col.name);
      }
      return formattedRow;
    });
  }

  private buildColumnNames(
    columns: ReadonlyArray<ColumnDefinition>,
  ): Record<string, string> {
    const columnNames: Record<string, string> = {};
    for (const col of columns) {
      columnNames[col.name] = String(col.title ?? col.name);
    }
    return columnNames;
  }

  private formatFilter(
    filter: DataGridFilter,
    columns: ReadonlyArray<ColumnDefinition>,
  ) {
    // Find the column definition to get the title
    const column = columns.find((c) => c.name === filter.column);
    const columnDisplay =
      column?.title !== undefined ? String(column.title) : filter.column;

    if ('value' in filter) {
      // Handle array values
      if (Array.isArray(filter.value)) {
        if (filter.value.length > 3) {
          return `${columnDisplay} ${filter.op} (${filter.value.length} values)`;
        } else {
          return `${columnDisplay} ${filter.op} (${filter.value.join(', ')})`;
        }
      }
      return `${columnDisplay} ${filter.op} ${filter.value}`;
    } else {
      return `${columnDisplay} ${filter.op}`;
    }
  }

  private getOrderedColumns(
    columns: ReadonlyArray<ColumnDefinition>,
    order: ColumnOrder,
  ): ReadonlyArray<ColumnDefinition> {
    // Create a map for fast lookup
    const columnMap = new Map(columns.map((c) => [c.name, c]));

    // Return ONLY columns in the specified order
    // Columns not in order are considered hidden
    const ordered = order.map((name) => columnMap.get(name)).filter(exists);

    return ordered;
  }

  private reorderColumns(
    currentOrder: ColumnOrder,
    fromKey: string | number | undefined,
    toKey: string | number | undefined,
    position: 'before' | 'after',
  ): ColumnOrder {
    if (typeof fromKey !== 'string' || typeof toKey !== 'string') {
      return currentOrder;
    }

    const newOrder = [...currentOrder];
    const fromIndex = newOrder.indexOf(fromKey);
    const toIndex = newOrder.indexOf(toKey);

    if (fromIndex === -1 || toIndex === -1) return currentOrder;

    // Can't drag a column relative to itself
    if (fromKey === toKey) return currentOrder;

    // Remove from old position
    newOrder.splice(fromIndex, 1);

    // Calculate new position
    let insertIndex = toIndex;
    if (fromIndex < toIndex) insertIndex--;
    if (position === 'after') insertIndex++;

    // Insert at new position
    newOrder.splice(insertIndex, 0, fromKey);

    return newOrder;
  }

  private reorderPivotColumns(
    pivot: PivotModel,
    fromKey: string | number | undefined,
    toKey: string | number | undefined,
    position: 'before' | 'after',
  ): PivotModel {
    if (typeof fromKey !== 'string' || typeof toKey !== 'string') {
      return pivot;
    }

    // Can't drag a column relative to itself
    if (fromKey === toKey) return pivot;

    const groupBy = [...pivot.groupBy];
    const valueKeys = Object.keys(pivot.values);

    const fromInGroupBy = groupBy.indexOf(fromKey);
    const toInGroupBy = groupBy.indexOf(toKey);
    const fromInValues = valueKeys.indexOf(fromKey);
    const toInValues = valueKeys.indexOf(toKey);

    // Case 1: Reordering within groupBy columns
    if (fromInGroupBy !== -1 && toInGroupBy !== -1) {
      groupBy.splice(fromInGroupBy, 1);
      let insertIndex = toInGroupBy;
      if (fromInGroupBy < toInGroupBy) insertIndex--;
      if (position === 'after') insertIndex++;
      groupBy.splice(insertIndex, 0, fromKey);

      return {
        groupBy,
        values: pivot.values,
      };
    }

    // Case 2: Reordering within value columns
    if (fromInValues !== -1 && toInValues !== -1) {
      // Need to rebuild values object with new order
      const newValueKeys = [...valueKeys];
      newValueKeys.splice(fromInValues, 1);
      let insertIndex = toInValues;
      if (fromInValues < toInValues) insertIndex--;
      if (position === 'after') insertIndex++;
      newValueKeys.splice(insertIndex, 0, fromKey);

      // Rebuild values object in new order
      const newValues: {[key: string]: PivotValue} = {};
      for (const key of newValueKeys) {
        newValues[key] = pivot.values[key];
      }

      return {
        groupBy: pivot.groupBy,
        values: newValues,
      };
    }

    // Case 3: Moving from groupBy to values or vice versa is not supported
    // (would change the semantics of the column)
    return pivot;
  }
}

export function renderCell(value: SqlValue | undefined, columnName: string) {
  if (value === undefined) {
    return '';
  }
  if (value instanceof Uint8Array) {
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
  } else if (typeof value === 'number' || typeof value === 'bigint') {
    return value.toString();
  } else {
    return value;
  }
}

// Creates a symbol for the aggregation function.
function aggregationFunctionSymbol(func: AggregationFunction): string {
  if (func === 'COUNT') return '#';
  return func;
}
