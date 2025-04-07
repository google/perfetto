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
import {MenuDivider, MenuItem, PopupMenu} from '../../../../widgets/menu';
import {buildSqlQuery} from './query_builder';
import {Icons} from '../../../../base/semantic_icons';
import {sqliteString} from '../../../../base/string_utils';
import {
  ColumnType,
  Row,
  SqlValue,
} from '../../../../trace_processor/query_result';
import {Anchor} from '../../../../widgets/anchor';
import {BasicTable} from '../../../../widgets/basic_table';
import {Spinner} from '../../../../widgets/spinner';

import {
  LegacySqlTableFilterOptions,
  LegacySqlTableFilterLabel,
} from './render_cell_utils';
import {SqlTableState} from './state';
import {SqlTableDescription} from './table_description';
import {Form} from '../../../../widgets/form';
import {TextInput} from '../../../../widgets/text_input';
import {TableColumn, TableManager, tableColumnId} from './table_column';
import {SqlColumn, sqlColumnId} from './sql_column';
import {SelectColumnMenu} from './select_column_menu';
import {renderColumnIcon, renderSortMenuItems} from './table_header';

export interface SqlTableConfig {
  readonly state: SqlTableState;
  // For additional menu items to add to the column header menus
  readonly addColumnMenuItems?: (
    column: TableColumn,
    columnAlias: string,
  ) => m.Children;
  // For additional filter actions
  readonly extraAddFilterActions?: (
    op: string,
    column: string,
    value?: string,
  ) => void;
  readonly extraRemoveFilterActions?: (filterSqlStr: string) => void;
}

type AdditionalColumnMenuItems = Record<string, m.Children>;

function renderCell(
  column: TableColumn,
  row: Row,
  state: SqlTableState,
): m.Children {
  const {columns} = state.getCurrentRequest();
  const sqlValue = row[columns[sqlColumnId(column.column)]];

  const additionalValues: {[key: string]: SqlValue} = {};
  const supportingColumns: {[key: string]: SqlColumn} =
    column.supportingColumns?.() ?? {};
  for (const [key, col] of Object.entries(supportingColumns)) {
    additionalValues[key] = row[columns[sqlColumnId(col)]];
  }

  return column.renderCell(sqlValue, getTableManager(state), additionalValues);
}

export function columnTitle(column: TableColumn): string {
  if (column.getTitle !== undefined) {
    const title = column.getTitle();
    if (title !== undefined) return title;
  }
  return sqlColumnId(column.column);
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
      {label: 'Add column', icon: Icons.Add},
      attrs.table.renderAddColumnOptions((column) => {
        attrs.state.addColumn(column, this.index++);
      }),
    );
  }
}

interface ColumnFilterAttrs {
  filterOption: LegacySqlTableFilterLabel;
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

    const {op, requiresParam} = LegacySqlTableFilterOptions[filterOption];

    return m(
      MenuItem,
      {
        label: filterOption,
        // Filter options that do not need an input value will filter the
        // table directly when clicking on the menu item
        // (ex: IS NULL or IS NOT NULL)
        onclick: !requiresParam
          ? () => {
              state.filters.addFilter({
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

              state.filters.addFilter({
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

  renderAddColumnOptions(addColumn: (column: TableColumn) => void): m.Children {
    // We do not want to add columns which already exist, so we track the
    // columns which we are already showing here.
    // TODO(altimin): Theoretically a single table can have two different
    // arg_set_ids, so we should track (arg_set_id_column, arg_name) pairs here.
    const existingColumnIds = new Set<string>();

    for (const column of this.state.getSelectedColumns()) {
      existingColumnIds.add(tableColumnId(column));
    }

    return m(SelectColumnMenu, {
      columns: this.table.columns.map((column) => ({
        key: columnTitle(column),
        column,
      })),
      manager: getTableManager(this.state),
      existingColumnIds,
      onColumnSelected: addColumn,
    });
  }

  renderColumnFilterOptions(
    c: TableColumn,
  ): m.Vnode<ColumnFilterAttrs, unknown>[] {
    return Object.keys(LegacySqlTableFilterOptions).map((label) =>
      m(ColumnFilter, {
        filterOption: label as LegacySqlTableFilterLabel,
        columns: [c.column],
        state: this.state,
      }),
    );
  }

  renderColumnHeader(
    column: TableColumn,
    index: number,
    additionalColumnHeaderMenuItems?: m.Children,
  ) {
    const sorted = this.state.isSortedBy(column);

    return m(
      PopupMenu,
      {
        trigger: m(
          Anchor,
          {icon: renderColumnIcon(sorted)},
          columnTitle(column),
        ),
      },
      renderSortMenuItems(sorted, (direction) =>
        this.state.sortBy({column, direction}),
      ),
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
      additionalColumnHeaderMenuItems,
      // Menu items before divider apply to selected column
      m(MenuDivider),
      // Menu items after divider apply to entire table
      m(AddColumnMenuItem, {table: this, state: this.state, index}),
    );
  }

  getAdditionalColumnMenuItems(
    addColumnMenuItems?: (
      column: TableColumn,
      columnAlias: string,
    ) => m.Children,
  ) {
    if (addColumnMenuItems === undefined) return;

    const additionalColumnMenuItems: AdditionalColumnMenuItems = {};
    this.state.getSelectedColumns().forEach((column) => {
      const columnAlias =
        this.state.getCurrentRequest().columns[sqlColumnId(column.column)];

      additionalColumnMenuItems[columnAlias] = addColumnMenuItems(
        column,
        columnAlias,
      );
    });

    return additionalColumnMenuItems;
  }

  view({attrs}: m.Vnode<SqlTableConfig>) {
    const rows = this.state.getDisplayedRows();
    const additionalColumnMenuItems = this.getAdditionalColumnMenuItems(
      attrs.addColumnMenuItems,
    );

    const columns = this.state.getSelectedColumns();
    const columnDescriptors = columns.map((column, i) => {
      return {
        title: this.renderColumnHeader(
          column,
          i,
          additionalColumnMenuItems &&
            additionalColumnMenuItems[
              this.state.getCurrentRequest().columns[sqlColumnId(column.column)]
            ],
        ),
        render: (row: Row) => renderCell(column, row, this.state),
      };
    });

    return [
      m(
        BasicTable<Row>,
        {
          data: rows,
          columns: columnDescriptors,
          onreorder: (from: number, to: number) =>
            this.state.moveColumn(from, to),
        },
        this.state.isLoading() && m(Spinner),
        this.state.getQueryError() !== undefined &&
          m('.query-error', this.state.getQueryError()),
      ),
    ];
  }
}

export function getTableManager(state: SqlTableState): TableManager {
  return {
    filters: state.filters,
    trace: state.trace,
    getSqlQuery: (columns: {[key: string]: SqlColumn}) =>
      buildSqlQuery({
        table: state.config.name,
        columns,
        filters: state.filters.get(),
        orderBy: state.getOrderedBy(),
      }),
  };
}
