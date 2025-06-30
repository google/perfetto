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
import {Row, SqlValue} from '../../../trace_processor/query_result';
import {Button} from '../../../widgets/button';
import {downloadData} from '../../../base/download_utils';
import {Anchor} from '../../../widgets/anchor';
import {
  ColumnDefinition,
  DataGridDataSource,
  DataSourceResult,
  FilterDefinition,
  RowDef,
  Sorting,
  SortByColumn,
  AggregationFunction,
} from './common';
import {MenuDivider, MenuItem, PopupMenu} from '../../../widgets/menu';
import {Chip} from '../../../widgets/chip';
import {Icon} from '../../../widgets/icon';
import {Icons} from '../../../base/semantic_icons';
import {InMemoryDataSource} from './in_memory_data_source';
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
  readonly toolbarItems?: m.Children;
}

export class DataGrid implements m.ClassComponent<DataGridAttrs> {
  // Internal state
  private currentPage = 0;

  private sorting: Sorting = {direction: 'UNSORTED'};
  private filters: ReadonlyArray<FilterDefinition> = [];

  oninit({attrs}: m.Vnode<DataGridAttrs>) {
    if (attrs.initialSorting) {
      this.sorting = attrs.initialSorting;
    }

    if (attrs.initialFilters) {
      this.filters = attrs.initialFilters;
    }
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
      toolbarItems,
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

    return m(
      '.pf-data-grid',
      {className: classNames(fillHeight && 'pf-data-grid--fill-height')},
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
        toolbarItems,
      ),
      m('.pf-data-grid__table', [
        m(
          'table',
          this.renderTableHeader(
            columns,
            sorting,
            onSortingChangedWithReset,
            addFilter,
            cellRenderer,
            dataSource.rows?.aggregates,
          ),
          dataSource.rows &&
            this.renderTableBody(
              columns,
              dataSource.rows,
              filters,
              onFiltersChangedWithReset,
              cellRenderer,
              maxRowsPerPage,
            ),
        ),
      ]),
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
    toolbarItems: m.Children,
  ) {
    if (
      totalPages === 1 &&
      filters.length === 0 &&
      !Boolean(toolbarItems) &&
      showResetButton === false
    ) {
      return undefined;
    }

    return m('.pf-data-grid__toolbar', [
      toolbarItems,
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
      m(
        '.pf-data-grid__toolbar-filters',
        showFilters &&
          filters.map((filter) =>
            m(Chip, {
              className: 'pf-data-grid__filter-chip',
              title: 'Remove filter',
              label: this.formatFilter(filter),
              onclick: () => {
                const newFilters = filters.filter((f) => f !== filter);
                this.filters = newFilters;
                onFiltersChanged(newFilters);
                this.currentPage = 0;
              },
            }),
          ),
      ),
      m('.pf-data-grid__toolbar-pagination', [
        m(Button, {
          icon: Icons.FirstPage,
          disabled: this.currentPage === 0,
          onclick: () => {
            if (this.currentPage !== 0) {
              this.currentPage = 0;
            }
          },
        }),
        m(Button, {
          icon: Icons.PrevPage,
          disabled: this.currentPage === 0,
          onclick: () => {
            if (this.currentPage > 0) {
              this.currentPage -= 1;
            }
          },
        }),
        m(
          'span.pf-data-grid__toolbar-page',
          this.renderPageInfo(this.currentPage, maxRowsPerPage, totalRows),
        ),
        m(Button, {
          icon: Icons.NextPage,
          disabled: this.currentPage >= totalPages - 1,
          onclick: () => {
            if (this.currentPage < totalPages - 1) {
              this.currentPage += 1;
            }
          },
        }),
        m(Button, {
          icon: Icons.LastPage,
          disabled: this.currentPage >= totalPages - 1,
          onclick: () => {
            if (this.currentPage < totalPages - 1) {
              this.currentPage = Math.max(0, totalPages - 1);
            }
          },
        }),
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

  private renderPageInfo(
    currentPage: number,
    maxRowsPerPage: number,
    totalRows: number,
  ): string {
    const startRow = Math.min(currentPage * maxRowsPerPage + 1, totalRows);
    const endRow = Math.min((currentPage + 1) * maxRowsPerPage, totalRows);

    const startRowStr = startRow.toLocaleString();
    const endRowStr = endRow.toLocaleString();
    const totalRowsStr = totalRows.toLocaleString();

    return `${startRowStr}-${endRowStr} of ${totalRowsStr}`;
  }

  private renderTableHeader(
    columns: ReadonlyArray<ColumnDefinition>,
    currentSortBy: Sorting,
    onSortingChanged: OnSortingChanged,
    addFilter: (filter: FilterDefinition) => void,
    cellRenderer: CellRenderer,
    aggregates?: Row,
  ) {
    const sortControls = onSortingChanged !== noOp;
    const filterControls = addFilter !== noOp;

    return m(
      'thead',
      m(
        'tr',
        columns.map((column) => {
          // Determine if this column is currently sorted
          const isCurrentSortColumn =
            currentSortBy.direction !== 'UNSORTED' &&
            (currentSortBy as SortByColumn).column === column.name;

          const currentDirection = isCurrentSortColumn
            ? (currentSortBy as SortByColumn).direction
            : undefined;

          return m(
            'th',
            m(
              '.pf-data-grid__data-with-btn.pf-data-grid__padded',
              m(
                'span',
                column.title ?? column.name,
                isCurrentSortColumn
                  ? currentDirection === 'ASC'
                    ? m(Icon, {icon: Icons.SortAsc})
                    : m(Icon, {icon: Icons.SortDesc})
                  : undefined,
              ),
              (sortControls || filterControls) &&
                m(
                  PopupMenu,
                  {
                    trigger: m(Button, {
                      className: 'pf-data-grid__cell-button',
                      icon: Icons.ContextMenuAlt,
                      compact: true,
                    }),
                  },
                  sortControls && [
                    (!isCurrentSortColumn || currentDirection === 'DESC') &&
                      m(MenuItem, {
                        label: 'Sort Ascending',
                        icon: Icons.SortAsc,
                        onclick: () => {
                          onSortingChanged({
                            column: column.name,
                            direction: 'ASC',
                          });
                        },
                      }),
                    (!isCurrentSortColumn || currentDirection === 'ASC') &&
                      m(MenuItem, {
                        label: 'Sort Descending',
                        icon: Icons.SortDesc,
                        onclick: () => {
                          onSortingChanged?.({
                            column: column.name,
                            direction: 'DESC',
                          });
                        },
                      }),
                    isCurrentSortColumn &&
                      m(MenuItem, {
                        label: 'Clear Sort',
                        icon: Icons.Remove,
                        onclick: () => {
                          onSortingChanged?.({
                            direction: 'UNSORTED',
                          });
                        },
                      }),
                  ],

                  filterControls && sortControls && m(MenuDivider),

                  filterControls && [
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
                  ],
                ),
            ),
            column.aggregation &&
              aggregates &&
              m('.pf-data-grid__aggregation.pf-data-grid__padded', [
                m(
                  'span',
                  {title: column.aggregation},
                  aggregationFunIcon(column.aggregation),
                ),
                cellRenderer(aggregates[column.name], column.name, aggregates),
                // If the context menu is present on the following cells, add a
                // spacer for one here too to keep the summary aligned with the
                // data below.
                filterControls &&
                  m(Button, {
                    className: 'pf-data-grid__hidden',
                    icon: Icons.ContextMenuAlt,
                    compact: true,
                  }),
              ]),
          );
        }),
      ),
    );
  }

  private renderTableBody(
    columns: ReadonlyArray<ColumnDefinition>,
    rowData: DataSourceResult,
    filters: ReadonlyArray<FilterDefinition>,
    onFilterChange: OnFiltersChanged,
    cellRenderer: CellRenderer,
    maxRowsPerPage: number,
  ) {
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

    return m(
      'tbody',
      indices.map((rowIndex) => {
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
            'tr',
            columns.map((column) => {
              const value = row[column.name];
              return m(
                'td',
                m(
                  '.pf-data-grid__data-with-btn.pf-data-grid__padded',
                  cellRenderer(value, column.name, row),
                  enableFilters &&
                    m(
                      PopupMenu,
                      {
                        trigger: m(Button, {
                          className: 'pf-data-grid__cell-button',
                          icon: Icons.ContextMenuAlt,
                          compact: true,
                        }),
                      },
                      value !== null && [
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
                      ],

                      isNumeric(value) && [
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
                      ],

                      value === null && [
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
                      ],
                    ),
                ),
              );
            }),
          );
        } else {
          // Return an empty placeholder row if data is not available
          return undefined;
        }
      }),
    );
  }
}

export function renderCell(value: SqlValue, columnName: string) {
  if (value instanceof Uint8Array) {
    return m(
      Anchor,
      {
        onclick: () => downloadData(`${columnName}.blob`, value),
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
