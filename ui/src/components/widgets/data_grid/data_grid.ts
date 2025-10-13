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
import {SqlValue} from '../../../trace_processor/query_result';
import {Button} from '../../../widgets/button';
import {download} from '../../../base/download_utils';
import {Anchor} from '../../../widgets/anchor';
import {
  ColumnDefinition,
  DataGridDataSource,
  DataSourceResult,
  FilterDefinition,
  RowDef,
  Sorting,
  AggregationFunction,
} from './common';
import {MenuDivider, MenuItem} from '../../../widgets/menu';
import {Icons} from '../../../base/semantic_icons';
import {InMemoryDataSource} from './in_memory_data_source';
import {Stack, StackAuto} from '../../../widgets/stack';
import {Box} from '../../../widgets/box';
import {LinearProgress} from '../../../widgets/linear_progress';
import {
  Grid,
  GridBody,
  GridDataCell,
  GridHeader,
  GridHeaderCell,
  GridRow,
  renderSortMenuItems,
  PageControl,
  SortDirection,
  GridFilterBar,
  GridFilterChip,
  GridAggregationCell,
} from '../../../widgets/grid';
import {classNames} from '../../../base/classnames';

const DEFAULT_ROWS_PER_PAGE = 50;

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

type OnFiltersChanged = (filters: ReadonlyArray<FilterDefinition>) => void;
type OnSortingChanged = (sorting: Sorting) => void;
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
  readonly onSortingChanged?: OnSortingChanged;

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
  readonly filters?: ReadonlyArray<FilterDefinition>;

  /**
   * Initial filters to apply to the grid on first load.
   * This is ignored in controlled mode (i.e. when `filters` is provided).
   */
  readonly initialFilters?: ReadonlyArray<FilterDefinition>;

  /**
   * Callback triggered when filters are added or removed.
   * Allows parent components to react to filtering changes.
   * Required for controlled mode filtering - when provided with filters,
   * the parent component becomes responsible for updating the filters prop.
   * @param filters The new array of filter definitions
   */
  readonly onFiltersChanged?: OnFiltersChanged;

  /**
   * Controls how many rows are displayed per page.
   */
  readonly maxRowsPerPage?: number;

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
}

export class DataGrid implements m.ClassComponent<DataGridAttrs> {
  // Internal state
  private currentPage = 0;

  private sorting: Sorting = {direction: 'UNSORTED'};
  private filters: ReadonlyArray<FilterDefinition> = [];
  private columnWidths: Map<string, number> = new Map();
  private hasCalculatedInitialWidths = false;

  oninit({attrs}: m.Vnode<DataGridAttrs>) {
    if (attrs.initialSorting) {
      this.sorting = attrs.initialSorting;
    }

    if (attrs.initialFilters) {
      this.filters = attrs.initialFilters;
    }

    // Initialize column widths with a default value
    attrs.columns.forEach((column) => {
      if (!this.columnWidths.has(column.name)) {
        this.columnWidths.set(column.name, 100);
      }
    });
  }

  view({attrs}: m.Vnode<DataGridAttrs>) {
    const {
      columns,
      data,
      sorting = this.sorting,
      onSortingChanged = sorting === this.sorting
        ? (x) => (this.sorting = x)
        : noOp,
      filters = this.filters,
      onFiltersChanged = filters === this.filters
        ? (x) => (this.filters = x)
        : noOp,
      cellRenderer = renderCell,
      maxRowsPerPage = DEFAULT_ROWS_PER_PAGE,
      showFiltersInToolbar = true,
      fillHeight = false,
      showResetButton = false,
      toolbarItemsLeft,
      toolbarItemsRight,
      className,
    } = attrs;

    const onFiltersChangedWithReset =
      onFiltersChanged === noOp
        ? noOp
        : (filter: ReadonlyArray<FilterDefinition>) => {
            onFiltersChanged(filter);
            this.currentPage = 0;
          };

    const onSortingChangedWithReset =
      onSortingChanged === noOp
        ? noOp
        : (sorting: Sorting) => {
            onSortingChanged(sorting);
            this.currentPage = 0;
          };

    // Initialize the datasource if required
    let dataSource: DataGridDataSource;
    if (Array.isArray(data)) {
      // If raw data supplied - just create a new in memory data source every
      // render cycle.
      dataSource = new InMemoryDataSource(data);
    } else {
      dataSource = data as DataGridDataSource;
    }

    // Work out the offset and limit and update the datasource
    const offset = this.currentPage * maxRowsPerPage;
    const limit = maxRowsPerPage;
    dataSource.notifyUpdate({
      columns: columns.map((c) => c.name),
      sorting,
      filters,
      pagination: {
        offset,
        limit,
      },
      aggregates: columns
        .filter((c) => c.aggregation)
        .map((c) => ({col: c.name, func: c.aggregation!})),
    });

    // Calculate initial column widths from first page of data
    if (
      !this.hasCalculatedInitialWidths &&
      dataSource.rows &&
      dataSource.rows.rows.length > 0
    ) {
      this.calculateInitialColumnWidths(
        columns,
        dataSource.rows.rows,
        cellRenderer,
      );
      this.hasCalculatedInitialWidths = true;
    }

    // Calculate total pages based on totalRows and rowsPerPage
    const totalRows = dataSource.rows?.totalRows ?? 0;
    const totalPages = Math.max(1, Math.ceil(totalRows / maxRowsPerPage));

    // Ensure current page doesn't exceed total pages
    if (this.currentPage >= totalPages && totalPages > 0) {
      this.currentPage = Math.max(0, totalPages - 1);
    }

    const addFilter =
      onFiltersChangedWithReset === noOp
        ? noOp
        : (filter: FilterDefinition) => onFiltersChanged([...filters, filter]);

    const sortControls = onSortingChangedWithReset !== noOp;
    const filterControls = onFiltersChangedWithReset !== noOp;

    return m(
      '.pf-data-grid',
      {
        className: classNames(
          fillHeight && 'pf-data-grid--fill-height',
          className,
        ),
      },
      this.renderTableToolbar(
        totalPages,
        totalRows,
        filters,
        sorting,
        onSortingChangedWithReset,
        onFiltersChangedWithReset,
        maxRowsPerPage,
        showFiltersInToolbar,
        showResetButton,
        toolbarItemsLeft,
        toolbarItemsRight,
      ),
      m(LinearProgress, {
        className: 'pf-data-grid__loading',
        state: dataSource.isLoading ? 'indeterminate' : 'none',
      }),
      m(
        Grid,
        {
          className: 'pf-data-grid__table',
        },
        [
          m(
            GridHeader,
            m(
              GridRow,
              columns.map((column) => {
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
                sortControls &&
                  menuItems.push(
                    renderSortMenuItems(sort, (direction) => {
                      if (direction) {
                        onSortingChangedWithReset({
                          column: column.name,
                          direction: direction,
                        });
                      } else {
                        onSortingChangedWithReset({
                          direction: 'UNSORTED',
                        });
                      }
                    }),
                  );

                if (filterControls && sortControls && menuItems.length > 0) {
                  menuItems.push(m(MenuDivider));
                }

                if (filterControls) {
                  menuItems.push(
                    m(MenuItem, {
                      label: 'Filter out nulls',
                      onclick: () => {
                        addFilter({column: column.name, op: 'is not null'});
                      },
                    }),
                    m(MenuItem, {
                      label: 'Only show nulls',
                      onclick: () => {
                        addFilter({column: column.name, op: 'is null'});
                      },
                    }),
                  );
                }

                return m(
                  GridHeaderCell,
                  {
                    sort,
                    onSort: sortControls
                      ? (direction: SortDirection) => {
                          onSortingChangedWithReset({
                            column: column.name,
                            direction,
                          });
                        }
                      : undefined,
                    menuItems: menuItems.length > 0 ? menuItems : undefined,
                    width: this.columnWidths.get(column.name) ?? 100,
                    onResize: (newWidth: number) => {
                      this.columnWidths.set(column.name, newWidth);
                      m.redraw();
                    },
                  },
                  column.title ?? column.name,
                );
              }),
            ),
            m(
              GridRow,
              columns.map((column) => {
                return m(
                  GridAggregationCell,
                  {
                    align: 'right', // Assume all aggregates are numeric
                    symbol:
                      column.aggregation &&
                      aggregationFunIcon(column.aggregation),
                    width: this.columnWidths.get(column.name) ?? 100,
                  },
                  column.aggregation &&
                    dataSource.rows?.aggregates &&
                    cellRenderer(
                      dataSource.rows.aggregates[column.name],
                      column.name,
                      dataSource.rows.aggregates,
                    ),
                );
              }),
            ),
          ),
          dataSource.rows &&
            m(
              GridBody,
              this.renderTableBody(
                columns,
                dataSource.rows,
                filters,
                onFiltersChangedWithReset,
                cellRenderer,
                maxRowsPerPage,
              ),
            ),
        ],
      ),
    );
  }

  private renderTableToolbar(
    totalPages: number,
    totalRows: number,
    filters: ReadonlyArray<FilterDefinition>,
    sorting: Sorting,
    onSortingChanged: OnSortingChanged,
    onFiltersChanged: OnFiltersChanged,
    maxRowsPerPage: number,
    showFilters: boolean,
    showResetButton: boolean,
    toolbarItemsLeft: m.Children,
    toolbarItemsRight: m.Children,
  ) {
    if (
      totalPages === 1 &&
      filters.length === 0 &&
      !(Boolean(toolbarItemsLeft) || Boolean(toolbarItemsRight)) &&
      showResetButton === false
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
              onSortingChanged({direction: 'UNSORTED'});
              onFiltersChanged([]);
            },
          }),
        m(StackAuto, [
          showFilters &&
            m(GridFilterBar, [
              filters.map((filter) => {
                return m(GridFilterChip, {
                  content: this.formatFilter(filter),
                  onRemove: () => {
                    const newFilters = filters.filter((f) => f !== filter);
                    this.filters = newFilters;
                    onFiltersChanged(newFilters);
                    this.currentPage = 0;
                  },
                });
              }),
            ]),
        ]),
        m(PageControl, {
          from: this.currentPage * maxRowsPerPage + 1,
          to: Math.min((this.currentPage + 1) * maxRowsPerPage, totalRows),
          of: totalRows,
          firstPageClick: () => {
            if (this.currentPage !== 0) {
              this.currentPage = 0;
            }
          },
          prevPageClick: () => {
            if (this.currentPage > 0) {
              this.currentPage -= 1;
            }
          },
          nextPageClick: () => {
            if (this.currentPage < totalPages - 1) {
              this.currentPage += 1;
            }
          },
          lastPageClick: () => {
            if (this.currentPage < totalPages - 1) {
              this.currentPage = Math.max(0, totalPages - 1);
            }
          },
        }),
        toolbarItemsRight,
      ]),
    ]);
  }

  private formatFilter(filter: FilterDefinition) {
    if ('value' in filter) {
      return `${filter.column} ${filter.op} ${filter.value}`;
    } else {
      return `${filter.column} ${filter.op}`;
    }
  }

  private renderTableBody(
    columns: ReadonlyArray<ColumnDefinition>,
    rowData: DataSourceResult,
    filters: ReadonlyArray<FilterDefinition>,
    onFilterChange: OnFiltersChanged,
    cellRenderer: CellRenderer,
    maxRowsPerPage: number,
  ): m.Children {
    const {rows, totalRows, rowOffset} = rowData;

    // Create array for all potential rows on the current page
    const startIndex = this.currentPage * maxRowsPerPage;
    const endIndex = Math.min(startIndex + maxRowsPerPage, totalRows);
    const displayRowCount = Math.max(0, endIndex - startIndex);
    const enableFilters = onFilterChange !== noOp;

    // Generate array of indices for rows that should be displayed
    const indices = Array.from(
      {length: displayRowCount},
      (_, i) => startIndex + i,
    );

    const addFilter = (x: FilterDefinition) => onFilterChange([...filters, x]);

    return indices.map((rowIndex) => {
      // Calculate the relative index within the available rows array
      const relativeIndex = rowIndex - rowOffset;
      // Check if this index is valid for the available rows
      const row =
        relativeIndex >= 0 && relativeIndex < rows.length
          ? rows[relativeIndex]
          : undefined;

      if (row) {
        // Return a populated row if data is available
        return m(
          GridRow,
          columns.map((column) => {
            const value = row[column.name];
            const menuItems: m.Children = [];

            if (enableFilters) {
              if (value !== null) {
                menuItems.push(
                  m(MenuItem, {
                    label: 'Filter equal to this',
                    onclick: () => {
                      addFilter({
                        column: column.name,
                        op: '=',
                        value: value,
                      });
                    },
                  }),
                  m(MenuItem, {
                    label: 'Filter not equal to this',
                    onclick: () => {
                      addFilter({
                        column: column.name,
                        op: '!=',
                        value: value,
                      });
                    },
                  }),
                );
              }

              if (isNumeric(value)) {
                menuItems.push(
                  m(MenuItem, {
                    label: 'Filter greater than this',
                    onclick: () => {
                      addFilter({
                        column: column.name,
                        op: '>',
                        value: value,
                      });
                    },
                  }),
                  m(MenuItem, {
                    label: 'Filter greater than or equal to this',
                    onclick: () => {
                      addFilter({
                        column: column.name,
                        op: '>=',
                        value: value,
                      });
                    },
                  }),
                  m(MenuItem, {
                    label: 'Filter less than this',
                    onclick: () => {
                      addFilter({
                        column: column.name,
                        op: '<',
                        value: value,
                      });
                    },
                  }),
                  m(MenuItem, {
                    label: 'Filter less than or equal to this',
                    onclick: () => {
                      addFilter({
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
                      addFilter({
                        column: column.name,
                        op: 'is not null',
                      });
                    },
                  }),
                  m(MenuItem, {
                    label: 'Only show nulls',
                    onclick: () => {
                      addFilter({
                        column: column.name,
                        op: 'is null',
                      });
                    },
                  }),
                );
              }
            }

            return m(
              GridDataCell,
              {
                menuItems: menuItems.length > 0 ? menuItems : undefined,
                align: (() => {
                  if (isNumeric(value)) return 'right';
                  if (value === null) return 'center';
                  return 'left';
                })(),
                nullish: value === null,
                width: this.columnWidths.get(column.name) ?? 100,
              },
              cellRenderer(value, column.name, row),
            );
          }),
        );
      } else {
        // Return an empty placeholder row if data is not available
        return undefined;
      }
    });
  }

  private calculateInitialColumnWidths(
    columns: ReadonlyArray<ColumnDefinition>,
    rows: ReadonlyArray<RowDef>,
    cellRenderer: CellRenderer,
  ) {
    // Create off-screen container for measuring with proper grid structure
    const measureContainer = document.createElement('div');
    measureContainer.style.position = 'absolute';
    measureContainer.style.visibility = 'hidden';
    measureContainer.style.pointerEvents = 'none';
    measureContainer.style.top = '-9999px';
    measureContainer.style.left = '-9999px';
    document.body.appendChild(measureContainer);

    columns.forEach((column) => {
      const widths: number[] = [];

      // Measure each cell in the column using actual GridDataCell component
      rows.forEach((row) => {
        const value = row[column.name];
        const cellContainer = document.createElement('div');

        // Render GridDataCell without menu items or width constraint
        const cellVnode = m(
          GridDataCell,
          {
            align: (() => {
              if (isNumeric(value)) return 'right';
              if (value === null) return 'center';
              return 'left';
            })(),
            nullish: value === null,
            width: 'fit-content',
          },
          cellRenderer(value, column.name, row),
        );

        m.render(cellContainer, cellVnode);
        measureContainer.appendChild(cellContainer);

        // Get the actual cell content width
        const cellElement = cellContainer.querySelector('.pf-grid__cell');
        if (cellElement) {
          widths.push(cellElement.scrollWidth);
        }

        measureContainer.removeChild(cellContainer);
      });

      // Measure header width using actual GridHeaderCell component
      const headerContainer = document.createElement('div');
      const headerVnode = m(
        GridHeaderCell,
        {width: 'fit-content'},
        column.title ?? column.name,
      );

      m.render(headerContainer, headerVnode);
      measureContainer.appendChild(headerContainer);

      const headerElement = headerContainer.querySelector('.pf-grid__cell');
      const headerWidth = headerElement ? headerElement.scrollWidth : 0;

      measureContainer.removeChild(headerContainer);

      // Calculate 95th percentile of cell widths
      if (widths.length > 0) {
        widths.sort((a, b) => a - b);
        const percentileIndex = Math.ceil(widths.length * 0.95) - 1;
        const width95 = widths[Math.min(percentileIndex, widths.length - 1)];

        // Take the maximum of 95th percentile and header width
        const finalWidth = Math.max(
          50,
          Math.ceil(Math.max(width95, headerWidth)),
        );
        this.columnWidths.set(column.name, finalWidth);
      } else {
        // If no cell data, just use header width
        const finalWidth = Math.max(50, Math.ceil(headerWidth));
        this.columnWidths.set(column.name, finalWidth);
      }
    });
    // Clean up
    document.body.removeChild(measureContainer);
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
  } else if (typeof value === 'number' || typeof value === 'bigint') {
    return m('span.pf-data-grid__cell--number', `${value}`);
  } else if (value === null) {
    return m('span.pf-data-grid__cell--null', 'null');
  } else {
    return m('span', `${value}`);
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
