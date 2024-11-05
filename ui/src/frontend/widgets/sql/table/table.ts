// Copyright (C) 2024 The Android Open Source Project
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
import {
  filterTitle,
  SqlColumn,
  sqlColumnId,
  TableColumn,
  tableColumnId,
  TableManager,
} from './column';
import {Button} from '../../../../widgets/button';
import {MenuDivider, MenuItem, PopupMenu2} from '../../../../widgets/menu';
import {buildSqlQuery} from './query_builder';
import {Icons} from '../../../../base/semantic_icons';
import {sqliteString} from '../../../../base/string_utils';
import {
  ColumnType,
  Row,
  SqlValue,
} from '../../../../trace_processor/query_result';
import {Anchor} from '../../../../widgets/anchor';
import {BasicTable, ReorderableColumns} from '../../../../widgets/basic_table';
import {Spinner} from '../../../../widgets/spinner';

import {ArgumentSelector} from './argument_selector';
import {FILTER_OPTION_TO_OP, FilterOption} from './render_cell_utils';
import {SqlTableState} from './state';
import {SqlTableDescription} from './table_description';
import {Intent} from '../../../../widgets/common';
import {addHistogramTab} from '../../charts/histogram/tab';
import {Form} from '../../../../widgets/form';
import {TextInput} from '../../../../widgets/text_input';

export interface SqlTableConfig {
  readonly state: SqlTableState;
}

function renderCell(
  column: TableColumn,
  row: Row,
  state: SqlTableState,
): m.Children {
  const {columns} = state.getCurrentRequest();
  const sqlValue = row[columns[sqlColumnId(column.primaryColumn())]];

  const additionalValues: {[key: string]: SqlValue} = {};
  const dependentColumns = column.dependentColumns?.() ?? {};
  for (const [key, col] of Object.entries(dependentColumns)) {
    additionalValues[key] = row[columns[sqlColumnId(col)]];
  }

  return column.renderCell(sqlValue, getTableManager(state), additionalValues);
}

function columnTitle(column: TableColumn): string {
  if (column.getTitle !== undefined) {
    const title = column.getTitle();
    if (title !== undefined) return title;
  }
  return sqlColumnId(column.primaryColumn());
}

interface AddColumnMenuItemAttrs {
  table: SqlTable;
  state: SqlTableState;
  index: number;
}

// This is separated into a separate class to store the index of the column to be
// added and increment it when multiple columns are added from the same popup menu.
class AddColumnMenuItem implements m.ClassComponent<AddColumnMenuItemAttrs> {
  // Index where the new column should be inserted.
  // In the regular case, a click would close the popup (destroying this class) and
  // the `index` would not change during its lifetime.
  // However, for mod-click, we want to keep adding columns to the right of the recently
  // added column, so to achieve that we keep track of the index and increment it for
  // each new column added.
  index: number;

  constructor({attrs}: m.Vnode<AddColumnMenuItemAttrs>) {
    this.index = attrs.index;
  }

  view({attrs}: m.Vnode<AddColumnMenuItemAttrs>) {
    return m(
      MenuItem,
      {label: 'Add column', icon: Icons.AddColumn},
      attrs.table.renderAddColumnOptions((column) => {
        attrs.state.addColumn(column, this.index++);
      }),
    );
  }
}

interface ColumnFilterAttrs {
  filterOption: FilterOption;
  columns: SqlColumn[];
  state: SqlTableState;
}

// Separating out an individual column filter into a class
// so that we can store the raw input value.
class ColumnFilter implements m.ClassComponent<ColumnFilterAttrs> {
  // Holds the raw string value from the filter text input element
  private inputValue: string;

  constructor() {
    this.inputValue = '';
  }

  view({attrs}: m.Vnode<ColumnFilterAttrs>) {
    const {filterOption, columns, state} = attrs;

    const {op, requiresParam} = FILTER_OPTION_TO_OP[filterOption];

    return m(
      MenuItem,
      {
        label: filterOption,
        // Filter options that do not need an input value will filter the
        // table directly when clicking on the menu item
        // (ex: IS NULL or IS NOT NULL)
        onclick: !requiresParam
          ? () => {
              state.addFilter({
                op: (cols) => `${cols[0]} ${op}`,
                columns,
              });
            }
          : undefined,
      },
      // All non-null filter options will have a submenu that allows
      // the user to enter a value into textfield and filter using
      // the Filter button.
      requiresParam &&
        m(
          Form,
          {
            onSubmit: () => {
              // Convert the string extracted from
              // the input text field into the correct data type for
              // filtering. The order in which each data type is
              // checked matters: string, number (floating), and bigint.
              if (this.inputValue === '') return;

              let filterValue: ColumnType;

              if (Number.isNaN(Number.parseFloat(this.inputValue))) {
                filterValue = sqliteString(this.inputValue);
              } else if (
                !Number.isInteger(Number.parseFloat(this.inputValue))
              ) {
                filterValue = Number(this.inputValue);
              } else {
                filterValue = BigInt(this.inputValue);
              }

              state.addFilter({
                op: (cols) => `${cols[0]} ${op} ${filterValue}`,
                columns,
              });
            },
            submitLabel: 'Filter',
          },
          m(TextInput, {
            id: 'column_filter_value',
            ref: 'COLUMN_FILTER_VALUE',
            autofocus: true,
            oninput: (e: KeyboardEvent) => {
              if (!e.target) return;

              this.inputValue = (e.target as HTMLInputElement).value;
            },
          }),
        ),
    );
  }
}

export class SqlTable implements m.ClassComponent<SqlTableConfig> {
  private readonly table: SqlTableDescription;

  private state: SqlTableState;

  constructor(vnode: m.Vnode<SqlTableConfig>) {
    this.state = vnode.attrs.state;
    this.table = this.state.config;
  }

  renderFilters(): m.Children {
    const filters: m.Child[] = [];
    for (const filter of this.state.getFilters()) {
      const label = filterTitle(filter);
      filters.push(
        m(Button, {
          label,
          icon: 'close',
          intent: Intent.Primary,
          onclick: () => {
            this.state.removeFilter(filter);
          },
        }),
      );
    }
    return filters;
  }

  renderAddColumnOptions(addColumn: (column: TableColumn) => void): m.Children {
    // We do not want to add columns which already exist, so we track the
    // columns which we are already showing here.
    // TODO(altimin): Theoretically a single table can have two different
    // arg_set_ids, so we should track (arg_set_id_column, arg_name) pairs here.
    const existingColumnIds = new Set<string>();

    for (const column of this.state.getSelectedColumns()) {
      existingColumnIds.add(tableColumnId(column));
    }

    const result = [];
    for (const column of this.table.columns) {
      if (column instanceof TableColumn) {
        if (existingColumnIds.has(tableColumnId(column))) continue;
        result.push(
          m(MenuItem, {
            label: columnTitle(column),
            onclick: () => addColumn(column),
          }),
        );
      } else {
        result.push(
          m(
            MenuItem,
            {
              label: column.getTitle(),
            },
            m(ArgumentSelector, {
              alreadySelectedColumnIds: existingColumnIds,
              tableManager: getTableManager(this.state),
              columnSet: column,
              onArgumentSelected: (column: TableColumn) => {
                addColumn(column);
              },
            }),
          ),
        );
        continue;
      }
    }
    return result;
  }

  renderColumnFilterOptions(
    c: TableColumn,
  ): m.Vnode<ColumnFilterAttrs, unknown>[] {
    return Object.values(FilterOption).map((filterOption) =>
      m(ColumnFilter, {
        filterOption,
        columns: [c.primaryColumn()],
        state: this.state,
      }),
    );
  }

  renderColumnHeader(column: TableColumn, index: number) {
    const sorted = this.state.isSortedBy(column);
    const icon =
      sorted === 'ASC'
        ? Icons.SortedAsc
        : sorted === 'DESC'
          ? Icons.SortedDesc
          : Icons.ContextMenu;

    return m(
      PopupMenu2,
      {
        trigger: m(Anchor, {icon}, columnTitle(column)),
      },
      sorted !== 'DESC' &&
        m(MenuItem, {
          label: 'Sort: highest first',
          icon: Icons.SortedDesc,
          onclick: () => {
            this.state.sortBy({
              column: column,
              direction: 'DESC',
            });
          },
        }),
      sorted !== 'ASC' &&
        m(MenuItem, {
          label: 'Sort: lowest first',
          icon: Icons.SortedAsc,
          onclick: () => {
            this.state.sortBy({
              column: column,
              direction: 'ASC',
            });
          },
        }),
      sorted !== undefined &&
        m(MenuItem, {
          label: 'Unsort',
          icon: Icons.Close,
          onclick: () => this.state.unsort(),
        }),
      this.state.getSelectedColumns().length > 1 &&
        m(MenuItem, {
          label: 'Hide',
          icon: Icons.Hide,
          onclick: () => this.state.hideColumnAtIndex(index),
        }),
      m(
        MenuItem,
        {label: 'Add filter', icon: Icons.Filter},
        this.renderColumnFilterOptions(column),
      ),
      m(MenuItem, {
        label: 'Create histogram',
        icon: Icons.Chart,
        onclick: () => {
          const columnAlias =
            this.state.getCurrentRequest().columns[
              sqlColumnId(column.primaryColumn())
            ];
          addHistogramTab({
            engine: this.state.trace.engine,
            sqlColumn: [columnAlias],
            columnTitle: columnTitle(column),
            filters: this.state.getFilters(),
            tableDisplay: this.table.displayName ?? this.table.name,
            query: this.state.getSqlQuery(
              Object.fromEntries([[columnAlias, column.primaryColumn()]]),
            ),
            aggregationType: column.aggregation?.().dataType,
          });
        },
      }),
      // Menu items before divider apply to selected column
      m(MenuDivider),
      // Menu items after divider apply to entire table
      m(AddColumnMenuItem, {table: this, state: this.state, index}),
    );
  }

  view() {
    const rows = this.state.getDisplayedRows();

    const columns = this.state.getSelectedColumns();
    const columnDescriptors = columns.map((column, i) => {
      return {
        title: this.renderColumnHeader(column, i),
        render: (row: Row) => renderCell(column, row, this.state),
      };
    });

    return [
      m('div', this.renderFilters()),
      m(
        BasicTable<Row>,
        {
          data: rows,
          columns: [
            new ReorderableColumns(
              columnDescriptors,
              (from: number, to: number) => this.state.moveColumn(from, to),
            ),
          ],
        },
        this.state.isLoading() && m(Spinner),
        this.state.getQueryError() !== undefined &&
          m('.query-error', this.state.getQueryError()),
      ),
    ];
  }
}

function getTableManager(state: SqlTableState): TableManager {
  return {
    addFilter: (filter) => {
      state.addFilter(filter);
    },
    trace: state.trace,
    getSqlQuery: (columns: {[key: string]: SqlColumn}) =>
      buildSqlQuery({
        table: state.config.name,
        columns,
        filters: state.getFilters(),
        orderBy: state.getOrderedBy(),
      }),
  };
}
