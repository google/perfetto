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
import {Box} from '../../../widgets/box';
import {Button} from '../../../widgets/button';
import {Chip} from '../../../widgets/chip';
import {LinearProgress} from '../../../widgets/linear_progress';
import {MenuDivider, MenuItem, MenuTitle} from '../../../widgets/menu';
import {Stack, StackAuto} from '../../../widgets/stack';
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
  RowDef,
  Sorting,
  ValueFormatter,
} from './common';
import {InMemoryDataSource} from './in_memory_data_source';
import {
  defaultValueFormatter,
  formatAsTSV,
  formatAsJSON,
  formatAsMarkdown,
} from './export_utils';
import {DataGridExportButton} from './export_buttons';

export class GridFilterBar implements m.ClassComponent {
  view({children}: m.Vnode) {
    return m(Stack, {orientation: 'horizontal', wrap: true}, children);
  }
}

export interface GridFilterAttrs {
  readonly content: string;
  onRemove(): void;
}

export class GridFilterChip implements m.ClassComponent<GridFilterAttrs> {
  view({attrs}: m.Vnode<GridFilterAttrs>): m.Children {
    return m(Chip, {
      className: 'pf-grid-filter',
      label: attrs.content,
      removable: true,
      onRemove: attrs.onRemove,
      title: attrs.content,
    });
  }
}

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
type OnFilterRemove = (index: number) => void;
type OnSortingChanged = (sorting: Sorting) => void;
type ColumnOrder = ReadonlyArray<string>;
type OnColumnOrderChanged = (columnOrder: ColumnOrder) => void;
type CellRenderer = (
  value: SqlValue,
  columnName: string,
  row: RowDef,
) => m.Children;

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
   * Optional custom cell renderer function.
   * Allows customization of how cell values are displayed.
   * @param value The raw value from the data source
   * @param columnName The name of the column being rendered
   * @param row The complete row data
   * @returns Renderable Mithril content for the cell
   */
  readonly cellRenderer?: CellRenderer;

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
   * Whether to show a 'reset' button on the toolbar, which resets filters and
   * sorting state. Default = false.
   */
  readonly showResetButton?: boolean;

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
  readonly showExportButtons?: boolean;

  /**
   * Optional value formatter for export. If not provided, uses the
   * cellRenderer to get displayed values. This is useful when you want
   * different formatting for export vs display (e.g., raw values vs formatted).
   */
  readonly valueFormatter?: ValueFormatter;

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
   * @param valueFormatter Optional custom formatter for values
   * @returns Promise<string> The formatted data as a string
   */
  exportData(
    format: 'tsv' | 'json' | 'markdown',
    valueFormatter?: ValueFormatter,
  ): Promise<string>;
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
  private dataGridApi: DataGridApi = {
    exportData: async (format, customFormatter) => {
      if (!this.currentDataSource || !this.currentColumns) {
        throw new Error('DataGrid not ready for export');
      }
      return await this.formatData(
        this.currentDataSource,
        this.currentColumns,
        customFormatter ?? this.currentValueFormatter,
        format,
      );
    },
  };
  private currentDataSource?: DataGridDataSource;
  private currentColumns?: ReadonlyArray<ColumnDefinition>;
  private currentValueFormatter?: ValueFormatter;

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
      columnOrder = this.columnOrder,
      onColumnOrderChanged = columnOrder === this.columnOrder
        ? (x) => (this.columnOrder = x)
        : noOp,
      columnReordering = onColumnOrderChanged !== noOp,
      cellRenderer = renderCell,
      showFiltersInToolbar = true,
      fillHeight = false,
      showResetButton = false,
      toolbarItemsLeft,
      toolbarItemsRight,
      className,
      showExportButtons = false,
      valueFormatter,
      onReady,
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
    });

    // Store current state for API access
    this.currentDataSource = dataSource;
    this.currentColumns = orderedColumns;
    this.currentValueFormatter = valueFormatter;

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

      const menuItems: m.Children = [];
      sortControls && menuItems.push(m(MenuTitle, {label: 'Sorting'}));
      sortControls &&
        menuItems.push(
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
        );

      if (filterControls && sortControls && menuItems.length > 0) {
        menuItems.push(m(MenuDivider));
        menuItems.push(m(MenuTitle, {label: 'Filters'}));
      }

      if (filterControls) {
        menuItems.push(
          m(MenuItem, {
            label: 'Filter out nulls',
            onclick: () => {
              onFilterAdd({column: column.name, op: 'is not null'});
            },
          }),
          m(MenuItem, {
            label: 'Only show nulls',
            onclick: () => {
              onFilterAdd({column: column.name, op: 'is null'});
            },
          }),
        );
      }

      if (Boolean(column.headerMenuItems)) {
        if (menuItems.length > 0) {
          menuItems.push(m(MenuDivider));
        }
        menuItems.push(column.headerMenuItems);
      }

      // Add column visibility options if column reordering is enabled
      if (columnReordering) {
        if (menuItems.length > 0) {
          menuItems.push(m(MenuDivider));
          menuItems.push(m(MenuTitle, {label: 'Column'}));
        }

        if (this.gridApi) {
          const gridApi = this.gridApi;
          menuItems.push(
            m(MenuItem, {
              label: 'Fit to content',
              icon: 'fit_width',
              onclick: () => gridApi.autoFitColumn(column.name),
            }),
          );
        }

        // Hide current column (only if more than 1 visible)
        if (orderedColumns.length > 1) {
          menuItems.push(
            m(MenuItem, {
              label: 'Hide',
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
        menuItems.push(
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
                return m(MenuItem, {
                  label: col.name,
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
      }

      // Build aggregation sub-content if needed
      const subContent =
        column.aggregation && dataSource.rows?.aggregates
          ? m(
              AggregationCell,
              {
                symbol: aggregationFunIcon(column.aggregation),
              },
              cellRenderer(
                dataSource.rows.aggregates[column.name],
                column.name,
                dataSource.rows.aggregates,
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
            menuItems: menuItems.length > 0 ? menuItems : undefined,
            subContent,
            label: column.name,
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
              if (value !== null) {
                menuItems.push(
                  m(MenuItem, {
                    label: 'Filter equal to this',
                    onclick: () => {
                      onFilterAdd({
                        column: column.name,
                        op: '=',
                        value: value,
                      });
                    },
                  }),
                  m(MenuItem, {
                    label: 'Filter not equal to this',
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

              // Add glob filter option for string columns with text selection
              if (typeof value === 'string') {
                const selectedText = window.getSelection()?.toString().trim();
                if (selectedText && selectedText.length > 0) {
                  menuItems.push(
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

              if (isNumeric(value)) {
                menuItems.push(
                  m(MenuItem, {
                    label: 'Filter greater than this',
                    onclick: () => {
                      onFilterAdd({
                        column: column.name,
                        op: '>',
                        value: value,
                      });
                    },
                  }),
                  m(MenuItem, {
                    label: 'Filter greater than or equal to this',
                    onclick: () => {
                      onFilterAdd({
                        column: column.name,
                        op: '>=',
                        value: value,
                      });
                    },
                  }),
                  m(MenuItem, {
                    label: 'Filter less than this',
                    onclick: () => {
                      onFilterAdd({
                        column: column.name,
                        op: '<',
                        value: value,
                      });
                    },
                  }),
                  m(MenuItem, {
                    label: 'Filter less than or equal to this',
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

              if (value === null) {
                menuItems.push(
                  m(MenuItem, {
                    label: 'Filter out nulls',
                    onclick: () => {
                      onFilterAdd({
                        column: column.name,
                        op: 'is not null',
                      });
                    },
                  }),
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

            // Add custom cell menu items if provided
            if (column.cellMenuItems !== undefined) {
              const extraItems = column.cellMenuItems(value, row);
              if (extraItems !== undefined) {
                if (menuItems.length > 0) {
                  menuItems.push(m(MenuDivider));
                }
                menuItems.push(extraItems);
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
                cellRenderer(value, column.name, row),
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
      this.renderTableToolbar(
        filters,
        sorting,
        onSort,
        onFilterRemove,
        showFiltersInToolbar,
        showResetButton,
        toolbarItemsLeft,
        toolbarItemsRight,
        showExportButtons,
      ),
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
      }),
    );
  }

  private renderTableToolbar(
    filters: ReadonlyArray<DataGridFilter>,
    sorting: Sorting,
    onSort: OnSortingChanged,
    onFilterRemove: OnFilterRemove,
    showFilters: boolean,
    showResetButton: boolean,
    toolbarItemsLeft: m.Children,
    toolbarItemsRight: m.Children,
    showExportButtons: boolean,
  ) {
    if (
      filters.length === 0 &&
      !(Boolean(toolbarItemsLeft) || Boolean(toolbarItemsRight)) &&
      showResetButton === false &&
      showExportButtons === false
    ) {
      return undefined;
    }

    return m(Box, {className: 'pf-data-grid__toolbar', spacing: 'small'}, [
      m(Stack, {orientation: 'horizontal', spacing: 'small'}, [
        toolbarItemsLeft,
        showResetButton &&
          m(Button, {
            icon: Icons.ResetState,
            label: 'Reset',
            disabled: filters.length === 0 && sorting.direction === 'UNSORTED',
            title: 'Reset grid state',
            onclick: () => {
              onSort({direction: 'UNSORTED'});
            },
          }),
        m(StackAuto, [
          showFilters &&
            m(GridFilterBar, [
              filters.map((filter) => {
                return m(GridFilterChip, {
                  content: this.formatFilter(filter),
                  onRemove: () => {
                    const filterIndex = filters.indexOf(filter);
                    onFilterRemove(filterIndex);
                  },
                });
              }),
            ]),
        ]),
        toolbarItemsRight,
        showExportButtons && m(DataGridExportButton, {api: this.dataGridApi}),
      ]),
    ]);
  }

  private async formatData(
    dataSource: DataGridDataSource,
    columns: ReadonlyArray<ColumnDefinition>,
    valueFormatter?: ValueFormatter,
    format: 'tsv' | 'json' | 'markdown' = 'tsv',
  ): Promise<string> {
    // Get all rows from the data source
    const rows = await dataSource.exportData();

    // Use provided formatter or default
    const formatter = valueFormatter ?? defaultValueFormatter;

    // Format the data based on the requested format
    switch (format) {
      case 'tsv':
        return this.formatAsTSV(rows, columns, formatter);
      case 'json':
        return this.formatAsJSON(rows, columns, formatter);
      case 'markdown':
        return this.formatAsMarkdown(rows, columns, formatter);
    }
  }

  private formatAsTSV(
    rows: readonly RowDef[],
    columns: ReadonlyArray<ColumnDefinition>,
    formatter: ValueFormatter,
  ): string {
    const formattedRows = this.formatRows(rows, columns, formatter);
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
    formatter: ValueFormatter,
  ): string {
    const formattedRows = this.formatRows(rows, columns, formatter);
    return formatAsJSON(formattedRows);
  }

  private formatAsMarkdown(
    rows: readonly RowDef[],
    columns: ReadonlyArray<ColumnDefinition>,
    formatter: ValueFormatter,
  ): string {
    const formattedRows = this.formatRows(rows, columns, formatter);
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
    formatter: ValueFormatter,
  ): Array<Record<string, string>> {
    return rows.map((row) => {
      const formattedRow: Record<string, string> = {};
      for (const col of columns) {
        const value = row[col.name];
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

  private formatFilter(filter: DataGridFilter) {
    if ('value' in filter) {
      return `${filter.column} ${filter.op} ${filter.value}`;
    } else {
      return `${filter.column} ${filter.op}`;
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
