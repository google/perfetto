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
import {FuzzyFinder} from '../../../base/fuzzy';
import {Icons} from '../../../base/semantic_icons';
import {exists} from '../../../base/utils';
import {SqlValue} from '../../../trace_processor/query_result';
import {Anchor} from '../../../widgets/anchor';
import {Button, ButtonVariant} from '../../../widgets/button';
import {EmptyState} from '../../../widgets/empty_state';
import {Form} from '../../../widgets/form';
import {Icon} from '../../../widgets/icon';
import {LinearProgress} from '../../../widgets/linear_progress';
import {MenuDivider, MenuItem} from '../../../widgets/menu';
import {TextInput} from '../../../widgets/text_input';
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
  ColumnDefinition,
  DataGridDataSource,
  DataGridFilter,
  DEFAULT_SUPPORTED_FILTERS,
  FilterType,
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

export interface AggregationCellAttrs extends m.Attributes {
  readonly symbol?: string;
}

export class AggregationCell implements m.ClassComponent<AggregationCellAttrs> {
  view({attrs, children}: m.Vnode<AggregationCellAttrs>) {
    const {className, symbol, ...rest} = attrs;
    return m(
      '.pf-aggr-cell',
      {
        ...rest,
        className: classNames(className),
      },
      m('.pf-aggr-cell__symbol', symbol),
      m('.pf-aggr-cell__content', children),
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

type OnFilterAdd = (filter: DataGridFilter) => void;
export type OnFilterRemove = (index: number) => void;
type OnSortingChanged = (sorting: Sorting) => void;
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
      columnOrder = this.columnOrder,
      onColumnOrderChanged = columnOrder === this.columnOrder
        ? (x) => (this.columnOrder = x)
        : noOp,
      columnReordering = onColumnOrderChanged !== noOp,
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

    // Reorder columns based on columnOrder array
    const orderedColumns = this.getOrderedColumns(columns, columnOrder);

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
      aggregates: columns
        .filter((c) => c.aggregation)
        .map((c) => ({col: c.name, func: c.aggregation!})),
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

    // Build VirtualGrid columns with all DataGrid features
    const virtualGridColumns = orderedColumns.map((column) => {
      const sort = (() => {
        if (sorting.direction === 'UNSORTED') {
          return undefined;
        } else if (sorting.column === column.name) {
          return sorting.direction;
        } else {
          return undefined;
        }
      })();

      // Build default menu groups
      const defaultGroups: {
        sorting?: m.Children;
        filters?: m.Children;
        fitToContent?: m.Children;
        columnManagement?: m.Children;
      } = {};

      // Sorting group
      if (sortControls) {
        defaultGroups.sorting = [
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
        ];
      }

      // Filters group
      if (filterControls) {
        const distinctState = dataSource.rows?.distinctValues?.get(column.name);

        // Build filter submenu - just add dividers freely, CSS will clean them up
        const filterSubmenuItems: m.Children = [
          // Null filters
          supportedFilters.includes('is not null') &&
            m(MenuItem, {
              label: 'Filter out nulls',
              onclick: () => {
                onFilterAdd({column: column.name, op: 'is not null'});
              },
            }),
          supportedFilters.includes('is null') &&
            m(MenuItem, {
              label: 'Only show nulls',
              onclick: () => {
                onFilterAdd({column: column.name, op: 'is null'});
              },
            }),
          m(MenuDivider),
          // Value-based filters for columns with distinct values enabled
          (column.distinctValues ?? true) &&
            supportedFilters.includes('in') &&
            m(
              MenuItem,
              {
                label: 'Equals to...',
                onChange: (isOpen) => {
                  if (isOpen === true) {
                    this.distinctValuesColumns.add(column.name);
                  } else {
                    this.distinctValuesColumns.delete(column.name);
                  }
                },
              },
              m(DistinctValuesSubmenu, {
                columnName: column.name,
                distinctState,
                formatValue: this.formatDistinctValue.bind(this),
                onApply: (selectedValues) => {
                  onFilterAdd({
                    column: column.name,
                    op: 'in',
                    value: Array.from(selectedValues),
                  });
                },
              }),
            ),
          (column.distinctValues ?? true) &&
            supportedFilters.includes('not in') &&
            m(
              MenuItem,
              {
                label: 'Not equals to...',
                onChange: (isOpen) => {
                  if (isOpen === true) {
                    this.distinctValuesColumns.add(column.name);
                  } else {
                    this.distinctValuesColumns.delete(column.name);
                  }
                },
              },
              m(DistinctValuesSubmenu, {
                columnName: column.name,
                distinctState,
                formatValue: this.formatDistinctValue.bind(this),
                onApply: (selectedValues) => {
                  onFilterAdd({
                    column: column.name,
                    op: 'not in',
                    value: Array.from(selectedValues),
                  });
                },
              }),
            ),
          m(MenuDivider),
          // Free-text equals/not equals filters for columns without distinct values
          !(column.distinctValues ?? true) &&
            supportedFilters.includes('=') &&
            m(
              MenuItem,
              {
                label: 'Equals to...',
              },
              m(TextFilterSubmenu, {
                columnName: column.name,
                operator: '=',
                onApply: (value) => {
                  onFilterAdd({
                    column: column.name,
                    op: '=',
                    value,
                  });
                },
              }),
            ),
          !(column.distinctValues ?? true) &&
            supportedFilters.includes('!=') &&
            m(
              MenuItem,
              {
                label: 'Not equals to...',
              },
              m(TextFilterSubmenu, {
                columnName: column.name,
                operator: '!=',
                onApply: (value) => {
                  onFilterAdd({
                    column: column.name,
                    op: '!=',
                    value,
                  });
                },
              }),
            ),
          m(MenuDivider),
          // Numeric comparison filters (only for numeric columns)
          column.filterType === 'numeric' &&
            supportedFilters.includes('>') &&
            m(
              MenuItem,
              {
                label: 'Greater than...',
              },
              m(TextFilterSubmenu, {
                columnName: column.name,
                operator: '>',
                onApply: (value) => {
                  onFilterAdd({
                    column: column.name,
                    op: '>',
                    value,
                  });
                },
              }),
            ),
          column.filterType === 'numeric' &&
            supportedFilters.includes('>=') &&
            m(
              MenuItem,
              {
                label: 'Greater than or equal...',
              },
              m(TextFilterSubmenu, {
                columnName: column.name,
                operator: '>=',
                onApply: (value) => {
                  onFilterAdd({
                    column: column.name,
                    op: '>=',
                    value,
                  });
                },
              }),
            ),
          column.filterType === 'numeric' &&
            supportedFilters.includes('<') &&
            m(
              MenuItem,
              {
                label: 'Less than...',
              },
              m(TextFilterSubmenu, {
                columnName: column.name,
                operator: '<',
                onApply: (value) => {
                  onFilterAdd({
                    column: column.name,
                    op: '<',
                    value,
                  });
                },
              }),
            ),
          column.filterType === 'numeric' &&
            supportedFilters.includes('<=') &&
            m(
              MenuItem,
              {
                label: 'Less than or equal...',
              },
              m(TextFilterSubmenu, {
                columnName: column.name,
                operator: '<=',
                onApply: (value) => {
                  onFilterAdd({
                    column: column.name,
                    op: '<=',
                    value,
                  });
                },
              }),
            ),
          m(MenuDivider),
          // Text-based filters (only if filterType is not 'numeric')
          column.filterType !== 'numeric' &&
            supportedFilters.includes('glob') &&
            m(
              MenuItem,
              {
                label: 'Contains...',
              },
              m(TextFilterSubmenu, {
                columnName: column.name,
                operator: 'contains',
                onApply: (value) => {
                  onFilterAdd({
                    column: column.name,
                    op: 'glob',
                    value: toCaseInsensitiveGlob(String(value)),
                  });
                },
              }),
            ),
          column.filterType !== 'numeric' &&
            supportedFilters.includes('not glob') &&
            m(
              MenuItem,
              {
                label: 'Not contains...',
              },
              m(TextFilterSubmenu, {
                columnName: column.name,
                operator: 'not contains',
                onApply: (value) => {
                  onFilterAdd({
                    column: column.name,
                    op: 'not glob',
                    value: toCaseInsensitiveGlob(String(value)),
                  });
                },
              }),
            ),
          column.filterType !== 'numeric' &&
            supportedFilters.includes('glob') &&
            m(
              MenuItem,
              {
                label: 'Glob...',
              },
              m(TextFilterSubmenu, {
                columnName: column.name,
                operator: 'glob',
                onApply: (value) => {
                  onFilterAdd({column: column.name, op: 'glob', value});
                },
              }),
            ),
          column.filterType !== 'numeric' &&
            supportedFilters.includes('not glob') &&
            m(
              MenuItem,
              {
                label: 'Not glob...',
              },
              m(TextFilterSubmenu, {
                columnName: column.name,
                operator: 'not glob',
                onApply: (value) => {
                  onFilterAdd({column: column.name, op: 'not glob', value});
                },
              }),
            ),
        ];

        // Only set filters group if there are any filter options
        // (filterSubmenuItems will be empty array if all conditions are false)
        if (filterSubmenuItems.some((item) => item !== false)) {
          defaultGroups.filters = [
            m(
              MenuItem,
              {label: 'Add filter...', icon: Icons.Filter},
              filterSubmenuItems,
            ),
          ];
        }
      }

      // Fit to content button (separate from column management)
      if (this.gridApi) {
        const gridApi = this.gridApi;
        defaultGroups.fitToContent = m(MenuItem, {
          label: 'Fit to content',
          icon: 'fit_width',
          onclick: () => gridApi.autoFitColumn(column.name),
        });
      }

      // Column management options
      const columnManagementItems: m.Children[] = [];

      // Hide current column (only if more than 1 visible)
      if (orderedColumns.length > 1) {
        columnManagementItems.push(
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
        );
      }

      const allColumnsShowing = columns.every((col) =>
        columnOrder.includes(col.name),
      );

      // Show/hide columns submenu
      columnManagementItems.push(
        m(
          MenuItem,
          {
            label: 'Manage columns',
            icon: 'view_column',
          },
          [
            // Show all
            m(MenuItem, {
              label: 'Show all',
              icon: allColumnsShowing ? Icons.Checkbox : Icons.BlankCheckbox,
              closePopupOnClick: false,
              onclick: () => {
                const newOrder = columns.map((c) => c.name);
                onColumnOrderChanged(newOrder);
              },
            }),
            m(MenuDivider),
            // Individual columns
            columns.map((col) => {
              const isVisible = columnOrder.includes(col.name);
              const columnLabel =
                col.title !== undefined ? String(col.title) : col.name;
              return m(MenuItem, {
                label: columnLabel,
                closePopupOnClick: false,
                icon: isVisible ? Icons.Checkbox : Icons.BlankCheckbox,
                onclick: () => {
                  if (isVisible) {
                    // Hide: remove from order (but keep at least 1 column)
                    if (columnOrder.length > 1) {
                      const newOrder = columnOrder.filter(
                        (name) => name !== col.name,
                      );
                      onColumnOrderChanged(newOrder);
                    }
                  } else {
                    // Show: add to end of order
                    const newOrder = [...columnOrder, col.name];
                    onColumnOrderChanged(newOrder);
                  }
                },
              });
            }),
          ],
        ),
      );

      if (columnManagementItems.length > 0) {
        defaultGroups.columnManagement = columnManagementItems;
      }

      // Build final menu items using contextMenuRenderer if provided
      const menuItems: m.Children = column.contextMenuRenderer
        ? column.contextMenuRenderer(defaultGroups)
        : [
            defaultGroups.sorting,
            m(MenuDivider),
            defaultGroups.filters,
            m(MenuDivider),
            defaultGroups.fitToContent,
            m(MenuDivider),
            defaultGroups.columnManagement,
          ];

      // Build aggregation sub-content if needed
      const subContent =
        column.aggregation && dataSource.rows?.aggregates
          ? m(
              AggregationCell,
              {
                symbol: aggregationFunIcon(column.aggregation),
              },
              column.cellRenderer
                ? column.cellRenderer(
                    dataSource.rows.aggregates[column.name],
                    dataSource.rows.aggregates,
                  )
                : renderCell(
                    dataSource.rows.aggregates[column.name],
                    column.name,
                  ),
            )
          : undefined;

      const gridColumn: GridColumn = {
        key: column.name,
        header: m(
          GridHeaderCell,
          {
            sort,
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
        reorderable: columnReordering
          ? {handle: 'datagrid-columns'}
          : undefined,
      };

      return gridColumn;
    });

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
            const menuItems: m.Children = [];

            // Build filter menu items if filtering is enabled
            if (filterControls) {
              const cellFilterItems: m.Children[] = [];

              if (value !== null) {
                if (supportedFilters.includes('=')) {
                  cellFilterItems.push(
                    m(MenuItem, {
                      label: 'Equal to this',
                      onclick: () => {
                        onFilterAdd({
                          column: column.name,
                          op: '=',
                          value: value,
                        });
                      },
                    }),
                  );
                }
                if (supportedFilters.includes('!=')) {
                  cellFilterItems.push(
                    m(MenuItem, {
                      label: 'Not equal to this',
                      onclick: () => {
                        onFilterAdd({
                          column: column.name,
                          op: '!=',
                          value: value,
                        });
                      },
                    }),
                  );
                }
              }

              // Add glob filter option for string columns with text selection
              // Only show if filterType is not 'numeric'
              if (
                typeof value === 'string' &&
                supportedFilters.includes('glob') &&
                column.filterType !== 'numeric'
              ) {
                const selectedText = window.getSelection()?.toString().trim();
                if (selectedText && selectedText.length > 0) {
                  cellFilterItems.push(
                    m(
                      MenuItem,
                      {
                        label: 'Filter glob',
                      },
                      m(MenuItem, {
                        label: `"${selectedText}*"`,
                        onclick: () => {
                          onFilterAdd({
                            column: column.name,
                            op: 'glob',
                            value: `${selectedText}*`,
                          });
                        },
                      }),
                      m(MenuItem, {
                        label: `"*${selectedText}"`,
                        onclick: () => {
                          onFilterAdd({
                            column: column.name,
                            op: 'glob',
                            value: `*${selectedText}`,
                          });
                        },
                      }),
                      m(MenuItem, {
                        label: `"*${selectedText}*"`,
                        onclick: () => {
                          onFilterAdd({
                            column: column.name,
                            op: 'glob',
                            value: `*${selectedText}*`,
                          });
                        },
                      }),
                    ),
                  );
                }
              }

              // Numeric comparison filters - only show if filterType is not 'string'
              if (isNumeric(value) && column.filterType !== 'string') {
                if (supportedFilters.includes('>')) {
                  cellFilterItems.push(
                    m(MenuItem, {
                      label: 'Greater than this',
                      onclick: () => {
                        onFilterAdd({
                          column: column.name,
                          op: '>',
                          value: value,
                        });
                      },
                    }),
                  );
                }
                if (supportedFilters.includes('>=')) {
                  cellFilterItems.push(
                    m(MenuItem, {
                      label: 'Greater than or equal to this',
                      onclick: () => {
                        onFilterAdd({
                          column: column.name,
                          op: '>=',
                          value: value,
                        });
                      },
                    }),
                  );
                }
                if (supportedFilters.includes('<')) {
                  cellFilterItems.push(
                    m(MenuItem, {
                      label: 'Less than this',
                      onclick: () => {
                        onFilterAdd({
                          column: column.name,
                          op: '<',
                          value: value,
                        });
                      },
                    }),
                  );
                }
                if (supportedFilters.includes('<=')) {
                  cellFilterItems.push(
                    m(MenuItem, {
                      label: 'Less than or equal to this',
                      onclick: () => {
                        onFilterAdd({
                          column: column.name,
                          op: '<=',
                          value: value,
                        });
                      },
                    }),
                  );
                }
              }

              if (value === null) {
                if (supportedFilters.includes('is not null')) {
                  cellFilterItems.push(
                    m(MenuItem, {
                      label: 'Filter out nulls',
                      onclick: () => {
                        onFilterAdd({
                          column: column.name,
                          op: 'is not null',
                        });
                      },
                    }),
                  );
                }
                if (supportedFilters.includes('is null')) {
                  cellFilterItems.push(
                    m(MenuItem, {
                      label: 'Only show nulls',
                      onclick: () => {
                        onFilterAdd({
                          column: column.name,
                          op: 'is null',
                        });
                      },
                    }),
                  );
                }
              }

              // Build "Add filter..." menu item to pass to renderer
              const addFilterItem =
                cellFilterItems.length > 0
                  ? m(
                      MenuItem,
                      {label: 'Add filter...', icon: Icons.Filter},
                      cellFilterItems,
                    )
                  : undefined;

              // Use custom cell context menu renderer if provided
              if (column.cellContextMenuRenderer) {
                const customMenuItems = column.cellContextMenuRenderer(
                  value,
                  row,
                  {addFilter: addFilterItem},
                );
                if (customMenuItems !== undefined && customMenuItems !== null) {
                  menuItems.push(customMenuItems);
                }
              } else if (addFilterItem !== undefined) {
                // Use default: just add the filter menu
                menuItems.push(addFilterItem);
              }
            }

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
              const newOrder = this.reorderColumns(
                columnOrder,
                from,
                to,
                position,
              );
              onColumnOrderChanged(newOrder);
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

  private formatDistinctValue(value: SqlValue): string {
    if (value === null) {
      return 'NULL';
    }
    if (value instanceof Uint8Array) {
      return `Blob (${value.length} bytes)`;
    }
    return String(value);
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
        const formatter = col.cellFormatter ?? defaultValueFormatter;
        formattedRow[col.name] = formatter(value, row);
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
}

/**
 * Converts a string to a case-insensitive glob pattern.
 * For example: "abc" becomes "*[aA][bB][cC]*"
 */
function toCaseInsensitiveGlob(text: string): string {
  const pattern = text
    .split('')
    .map((char) => {
      const lower = char.toLowerCase();
      const upper = char.toUpperCase();
      // Only create character class for letters
      if (lower !== upper) {
        return `[${lower}${upper}]`;
      }
      // Non-letters remain as-is
      return char;
    })
    .join('');
  return `*${pattern}*`;
}

export function renderCell(value: SqlValue, columnName: string) {
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
  } else {
    return String(value);
  }
}

// Check if the value is numeric (number or bigint)
export function isNumeric(value: SqlValue): value is number | bigint {
  return typeof value === 'number' || typeof value === 'bigint';
}

// Creates a unicode icon for the aggregation function.
function aggregationFunIcon(func: AggregationFunction): string {
  switch (func) {
    case 'SUM':
      return 'Σ';
    case 'COUNT':
      return '#';
    case 'AVG':
      return '⌀';
    case 'MIN':
      return '↓';
    case 'MAX':
      return '↑';
    default:
      throw new Error(`Unknown aggregation function: ${func}`);
  }
}

// Helper component to manage distinct values selection
interface DistinctValuesSubmenuAttrs {
  readonly columnName: string;
  readonly distinctState: ReadonlyArray<SqlValue> | undefined;
  readonly formatValue: (value: SqlValue) => string;
  readonly onApply: (selectedValues: Set<SqlValue>) => void;
}

class DistinctValuesSubmenu
  implements m.ClassComponent<DistinctValuesSubmenuAttrs>
{
  private selectedValues = new Set<SqlValue>();
  private searchQuery = '';
  private static readonly MAX_VISIBLE_ITEMS = 100;

  view({attrs}: m.Vnode<DistinctValuesSubmenuAttrs>) {
    const {distinctState, formatValue, onApply} = attrs;

    if (distinctState === undefined) {
      return m('.pf-distinct-values-menu', [
        m(MenuItem, {label: 'Loading...', disabled: true}),
      ]);
    }

    // Use fuzzy search to filter and get highlighted segments
    const fuzzyResults = (() => {
      if (this.searchQuery === '') {
        // No search - show all values without highlighting
        return distinctState.map((value) => ({
          value,
          segments: [{matching: false, value: formatValue(value)}],
        }));
      } else {
        // Fuzzy search with highlighting
        const finder = new FuzzyFinder(distinctState, (v) => formatValue(v));
        return finder.find(this.searchQuery).map((result) => ({
          value: result.item,
          segments: result.segments,
        }));
      }
    })();

    // Limit the number of items rendered
    const visibleResults = fuzzyResults.slice(
      0,
      DistinctValuesSubmenu.MAX_VISIBLE_ITEMS,
    );
    const remainingCount =
      fuzzyResults.length - DistinctValuesSubmenu.MAX_VISIBLE_ITEMS;

    return m('.pf-distinct-values-menu', [
      m(
        '.pf-distinct-values-menu__search',
        {
          onclick: (e: MouseEvent) => {
            // Prevent menu from closing when clicking search box
            e.stopPropagation();
          },
        },
        m(TextInput, {
          placeholder: 'Search...',
          value: this.searchQuery,
          oninput: (e: InputEvent) => {
            this.searchQuery = (e.target as HTMLInputElement).value;
          },
          onkeydown: (e: KeyboardEvent) => {
            if (this.searchQuery !== '' && e.key === 'Escape') {
              this.searchQuery = '';
              e.stopPropagation(); // Prevent menu from closing
            }
          },
        }),
      ),
      m(
        '.pf-distinct-values-menu__list',
        fuzzyResults.length > 0
          ? [
              visibleResults.map((result) => {
                const isSelected = this.selectedValues.has(result.value);
                // Render highlighted label
                const labelContent = result.segments.map((segment) => {
                  if (segment.matching) {
                    return m('strong.pf-fuzzy-match', segment.value);
                  } else {
                    return segment.value;
                  }
                });

                // Render custom menu item with highlighted content
                return m(
                  'button.pf-menu-item',
                  {
                    onclick: () => {
                      if (isSelected) {
                        this.selectedValues.delete(result.value);
                      } else {
                        this.selectedValues.add(result.value);
                      }
                    },
                  },
                  m(Icon, {
                    className: 'pf-menu-item__left-icon',
                    icon: isSelected ? Icons.Checkbox : Icons.BlankCheckbox,
                  }),
                  m('.pf-menu-item__label', labelContent),
                );
              }),
              remainingCount > 0 &&
                m(MenuItem, {
                  label: `...and ${remainingCount} more`,
                  disabled: true,
                }),
            ]
          : m(EmptyState, {
              title: 'No matches',
            }),
      ),
      m('.pf-distinct-values-menu__footer', [
        m(MenuItem, {
          label: 'Apply',
          icon: 'check',
          disabled: this.selectedValues.size === 0,
          onclick: () => {
            if (this.selectedValues.size > 0) {
              onApply(this.selectedValues);
              this.selectedValues.clear();
              this.searchQuery = '';
            }
          },
        }),
        m(MenuItem, {
          label: 'Clear selection',
          icon: 'close',
          disabled: this.selectedValues.size === 0,
          closePopupOnClick: false,
          onclick: () => {
            this.selectedValues.clear();
            m.redraw();
          },
        }),
      ]),
    ]);
  }
}

// Helper component for text-based filter input
interface TextFilterSubmenuAttrs {
  readonly columnName: string;
  readonly operator:
    | 'glob'
    | 'not glob'
    | 'contains'
    | 'not contains'
    | '='
    | '!='
    | '>'
    | '>='
    | '<'
    | '<=';
  readonly onApply: (value: string | number) => void;
}

class TextFilterSubmenu implements m.ClassComponent<TextFilterSubmenuAttrs> {
  private inputValue = '';

  view({attrs}: m.Vnode<TextFilterSubmenuAttrs>) {
    const {operator, onApply} = attrs;

    const placeholder = (() => {
      switch (operator) {
        case 'glob':
          return 'Enter glob pattern (e.g., *text*)...';
        case 'not glob':
          return 'Enter glob pattern to exclude...';
        case 'contains':
          return 'Enter text to include...';
        case 'not contains':
          return 'Enter text to exclude...';
        case '=':
          return 'Enter value to match...';
        case '!=':
          return 'Enter value to exclude...';
        case '>':
          return 'Enter number...';
        case '>=':
          return 'Enter number...';
        case '<':
          return 'Enter number...';
        case '<=':
          return 'Enter number...';
      }
    })();

    // Check if this is a numeric comparison operator
    const isNumericOperator = ['>', '>=', '<', '<='].includes(operator);

    const applyFilter = () => {
      if (this.inputValue.trim().length > 0) {
        let value: string | number = this.inputValue.trim();

        // For numeric operators, try to parse as number
        if (isNumericOperator) {
          const numValue = Number(value);
          if (!isNaN(numValue)) {
            value = numValue;
          }
        }

        onApply(value);
        this.inputValue = '';
      }
    };

    return m(
      Form,
      {
        className: 'pf-data-grid__text-filter-form',
        submitLabel: 'Add Filter',
        submitIcon: 'check',
        onSubmit: (e: Event) => {
          e.preventDefault();
          applyFilter();
        },
        validation: () => this.inputValue.trim().length > 0,
      },
      m(TextInput, {
        placeholder,
        value: this.inputValue,
        autofocus: true,
        oninput: (e: InputEvent) => {
          this.inputValue = (e.target as HTMLInputElement).value;
        },
      }),
    );
  }
}
