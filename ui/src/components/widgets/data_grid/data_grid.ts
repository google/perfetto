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
import {downloadData} from '../../../base/download_utils';
import {Anchor} from '../../../widgets/anchor';
import {
  ColumnDefinition,
  DataGridDataSource,
  DataSourceResult,
  FilterDefinition,
  RowDef,
  SortBy,
  SortByColumn,
} from './common';
import {MenuDivider, MenuItem, PopupMenu} from '../../../widgets/menu';
import {Chip} from '../../../widgets/chip';
import {Icon} from '../../../widgets/icon';
import {Icons} from '../../../base/semantic_icons';

const DEFAULT_ROWS_PER_PAGE = 50;

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
  readonly dataSource: DataGridDataSource;

  /**
   * Current sort configuration - can operate in controlled or uncontrolled
   * mode.
   *
   * In controlled mode: Provide this prop along with onSortByChange callback.
   * In uncontrolled mode: Omit this prop to let the grid manage sorting state
   * internally.
   *
   * Specifies which column to sort by and the direction (asc/desc/unsorted). If
   * not provided, defaults to internal state with direction 'unsorted'.
   */
  readonly sortBy?: SortBy;

  /**
   * Array of filters to apply to the data - can operate in controlled or
   * uncontrolled mode.
   *
   * In controlled mode: Provide this prop along with onFilterChange callback.
   * In uncontrolled mode: Omit this prop to let the grid manage filter state
   * internally.
   *
   * Each filter contains a column name, operator, and comparison value. If not
   * provided, defaults to an empty array (no filters initially applied).
   */
  readonly filters?: ReadonlyArray<FilterDefinition>;

  /**
   * Controls how many rows are displayed per page.
   */
  readonly maxRowsPerPage?: number;

  /**
   * Callback triggered when the sort configuration changes.
   * Allows parent components to react to sorting changes.
   * Required for controlled mode sorting - when provided with sortBy,
   * the parent component becomes responsible for updating the sortBy prop.
   * @param sortBy The new sort configuration
   */
  onSortByChange?(sortBy: SortBy): void;

  /**
   * Callback triggered when filters are added or removed.
   * Allows parent components to react to filtering changes.
   * Required for controlled mode filtering - when provided with filters,
   * the parent component becomes responsible for updating the filters prop.
   * @param filters The new array of filter definitions
   */
  onFilterChange?(filters: ReadonlyArray<FilterDefinition>): void;

  /**
   * Optional custom cell renderer function.
   * Allows customization of how cell values are displayed.
   * @param value The raw value from the data source
   * @param columnName The name of the column being rendered
   * @param row The complete row data
   * @returns Renderable Mithril content for the cell
   */
  cellRenderer?: (
    value: SqlValue,
    columnName: string,
    row: RowDef,
  ) => m.Children;

  /**
   * Display applied filters in the toolbar. Set to false to hide them, for
   * example, if filters are displayed elsewhere in the UI. This does not
   * disable filtering functionality.
   *
   * Defaults to true.
   */
  readonly showFiltersInToolbar?: boolean;
}

export class DataGrid implements m.ClassComponent<DataGridAttrs> {
  // Internal state
  private currentPage = 0;
  private internalSortBy: SortBy = {direction: 'unsorted'};
  private internalFilters: ReadonlyArray<FilterDefinition> = [];

  view({attrs}: m.Vnode<DataGridAttrs>) {
    const {
      columns,
      dataSource,
      sortBy: externalSorting,
      filters: externalFilters,
      onSortByChange,
      onFilterChange,
      cellRenderer,
      maxRowsPerPage = DEFAULT_ROWS_PER_PAGE,
      showFiltersInToolbar = true,
    } = attrs;

    // If filters are passed in from outside but no onFilterChange handler
    // specified, then there is no way to edit the filters so we hide the
    // options to specify filters.
    const areFiltersControlled = externalFilters !== undefined;
    const filters = areFiltersControlled
      ? externalFilters
      : this.internalFilters;

    // If filters are not controlled, they are always editable because the
    // filter state is stored internally so we don't need a callback to modify
    // the filters. If the filters are controlled and we have a callback then
    // filters are similarly editable, however if we don't have a callback then
    // filters cannot be changed so we consider them readonly.
    const filtersAreEditable =
      !areFiltersControlled || onFilterChange !== undefined;

    const isSortingControlled = externalSorting !== undefined;
    const sortBy = isSortingControlled ? externalSorting : this.internalSortBy;
    const sortingIsEditable =
      !isSortingControlled || onSortByChange !== undefined;

    const currentPage = this.currentPage;
    this.updateDataSource(
      dataSource,
      sortBy,
      filters,
      currentPage,
      maxRowsPerPage,
    );

    const rowData = dataSource.rows;
    const totalRows = rowData.totalRows;

    // Calculate total pages based on totalRows and rowsPerPage
    const totalPages = Math.max(1, Math.ceil(totalRows / maxRowsPerPage));

    // Ensure current page doesn't exceed total pages
    if (this.currentPage >= totalPages && totalPages > 0) {
      this.currentPage = Math.max(0, totalPages - 1);
    }

    const addFilter = filtersAreEditable
      ? (filter: FilterDefinition) =>
          this.addFilter(filters, filter, onFilterChange)
      : undefined;

    const updateSorting = sortingIsEditable
      ? (sortBy: SortBy) => {
          this.internalSortBy = sortBy;
          onSortByChange?.(sortBy);
        }
      : undefined;

    return m(
      '.pf-data-grid',
      this.renderTableToolbar(
        totalPages,
        totalRows,
        filters,
        sortBy,
        onSortByChange,
        onFilterChange,
        maxRowsPerPage,
        showFiltersInToolbar,
      ),
      m(
        'table',
        this.renderTableHeader(columns, sortBy, updateSorting, addFilter),
        this.renderTableBody(
          columns,
          rowData,
          filtersAreEditable,
          filters,
          onFilterChange,
          cellRenderer,
          maxRowsPerPage,
        ),
      ),
    );
  }

  private updateDataSource(
    dataSource: DataGridDataSource,
    sortBy: SortBy,
    filters: ReadonlyArray<FilterDefinition>,
    currentPage: number,
    maxRowsPerPage: number,
  ) {
    const offset = currentPage * maxRowsPerPage;
    const limit = maxRowsPerPage;
    dataSource.notifyUpdate(sortBy, filters, offset, limit);
  }

  private renderTableToolbar(
    totalPages: number,
    totalRows: number,
    filters: ReadonlyArray<FilterDefinition>,
    sortBy: SortBy,
    onSortByChange: ((sortBy: SortBy) => void) | undefined,
    onFiltersChange:
      | ((filters: ReadonlyArray<FilterDefinition>) => void)
      | undefined,
    maxRowsPerPage: number,
    showFilters: boolean,
  ) {
    return m('.pf-data-grid__toolbar', [
      m(Button, {
        icon: Icons.ResetState,
        label: 'Reset',
        disabled: filters.length === 0 && sortBy.direction === 'unsorted',
        title: 'Reset filters and sorting',
        onclick: () => {
          const newSortBy: SortBy = {direction: 'unsorted'};
          this.internalSortBy = newSortBy;
          onSortByChange?.(newSortBy);

          const newFilters: ReadonlyArray<FilterDefinition> = [];
          this.internalFilters = newFilters;
          onFiltersChange?.(newFilters);
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
                this.internalFilters = newFilters;
                onFiltersChange?.(newFilters);
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
    currentSortBy: SortBy,
    updateSorting: ((sortBy: SortBy) => void) | undefined,
    addFilter: ((filter: FilterDefinition) => void) | undefined,
  ) {
    return m(
      'thead',
      m(
        'tr',
        columns.map((column) => {
          // Determine if this column is currently sorted
          const isCurrentSortColumn =
            currentSortBy.direction !== 'unsorted' &&
            (currentSortBy as SortByColumn).column === column.name;

          const currentDirection = isCurrentSortColumn
            ? (currentSortBy as SortByColumn).direction
            : undefined;

          return m(
            'th',
            m(
              '.pf-data-grid__cell',
              m(
                'span',
                column.name,
                isCurrentSortColumn
                  ? currentDirection === 'asc'
                    ? m(Icon, {icon: Icons.SortAsc})
                    : m(Icon, {icon: Icons.SortDesc})
                  : undefined,
              ),
              (updateSorting || addFilter) &&
                m(
                  PopupMenu,
                  {
                    trigger: m(Button, {
                      className: 'pf-data-grid__cell-button',
                      icon: Icons.ContextMenuAlt,
                      compact: true,
                    }),
                  },
                  updateSorting && [
                    (!isCurrentSortColumn || currentDirection === 'desc') &&
                      m(MenuItem, {
                        label: 'Sort Ascending',
                        icon: Icons.SortAsc,
                        onclick: () => {
                          updateSorting?.({
                            column: column.name,
                            direction: 'asc',
                          });
                        },
                      }),
                    (!isCurrentSortColumn || currentDirection === 'asc') &&
                      m(MenuItem, {
                        label: 'Sort Descending',
                        icon: Icons.SortDesc,
                        onclick: () => {
                          updateSorting?.({
                            column: column.name,
                            direction: 'desc',
                          });
                        },
                      }),
                    isCurrentSortColumn &&
                      m(MenuItem, {
                        label: 'Clear Sort',
                        icon: Icons.Remove,
                        onclick: () => {
                          updateSorting?.({
                            direction: 'unsorted',
                          });
                        },
                      }),
                  ],

                  addFilter && updateSorting && m(MenuDivider),

                  addFilter && [
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
          );
        }),
      ),
    );
  }

  private renderTableBody(
    columns: ReadonlyArray<ColumnDefinition>,
    rowData: DataSourceResult,
    enableFilters: boolean,
    filters: ReadonlyArray<FilterDefinition>,
    onFilterChange:
      | ((filters: ReadonlyArray<FilterDefinition>) => void)
      | undefined,
    cellRenderer:
      | ((value: SqlValue, columnName: string, row: RowDef) => m.Children)
      | undefined,
    maxRowsPerPage: number,
  ) {
    const {rows, totalRows, rowOffset} = rowData;

    // Create array for all potential rows on the current page
    const startIndex = this.currentPage * maxRowsPerPage;
    const endIndex = Math.min(startIndex + maxRowsPerPage, totalRows);
    const displayRowCount = Math.max(0, endIndex - startIndex);

    // Generate array of indices for rows that should be displayed
    const indices = Array.from(
      {length: displayRowCount},
      (_, i) => startIndex + i,
    );

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
                  '.pf-data-grid__cell',
                  cellRenderer
                    ? cellRenderer(value, column.name, row)
                    : renderCell(value, column.name),
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
                            this.addFilter(
                              filters,
                              {
                                column: column.name,
                                op: '=',
                                value: value,
                              },
                              onFilterChange,
                            );
                          },
                        }),
                        m(MenuItem, {
                          label: 'Filter not equal to this',
                          onclick: () => {
                            this.addFilter(
                              filters,
                              {
                                column: column.name,
                                op: '!=',
                                value: value,
                              },
                              onFilterChange,
                            );
                          },
                        }),
                      ],

                      isNumeric(value) && [
                        m(MenuItem, {
                          label: 'Filter greater than this',
                          onclick: () => {
                            this.addFilter(
                              filters,
                              {
                                column: column.name,
                                op: '>',
                                value: value,
                              },
                              onFilterChange,
                            );
                          },
                        }),
                        m(MenuItem, {
                          label: 'Filter greater than or equal to this',
                          onclick: () => {
                            this.addFilter(
                              filters,
                              {
                                column: column.name,
                                op: '>=',
                                value: value,
                              },
                              onFilterChange,
                            );
                          },
                        }),
                        m(MenuItem, {
                          label: 'Filter less than this',
                          onclick: () => {
                            this.addFilter(
                              filters,
                              {
                                column: column.name,
                                op: '<',
                                value: value,
                              },
                              onFilterChange,
                            );
                          },
                        }),
                        m(MenuItem, {
                          label: 'Filter less than or equal to this',
                          onclick: () => {
                            this.addFilter(
                              filters,
                              {
                                column: column.name,
                                op: '<=',
                                value: value,
                              },
                              onFilterChange,
                            );
                          },
                        }),
                      ],

                      value === null && [
                        m(MenuItem, {
                          label: 'Filter out nulls',
                          onclick: () => {
                            this.addFilter(
                              filters,
                              {
                                column: column.name,
                                op: 'is not null',
                              },
                              onFilterChange,
                            );
                          },
                        }),
                        m(MenuItem, {
                          label: 'Only show nulls',
                          onclick: () => {
                            this.addFilter(
                              filters,
                              {
                                column: column.name,
                                op: 'is null',
                              },
                              onFilterChange,
                            );
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
          return m(
            'tr',
            columns.map(() => m('td', m('.pf-data-grid__cell--loading', ''))),
          );
        }
      }),
    );
  }

  private addFilter(
    filters: ReadonlyArray<FilterDefinition>,
    newFilter: FilterDefinition,
    onFilterChange:
      | ((filters: ReadonlyArray<FilterDefinition>) => void)
      | undefined,
  ) {
    const newFilters = [...filters, newFilter];
    this.internalFilters = newFilters;
    this.currentPage = 0;
    onFilterChange?.(newFilters);
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
