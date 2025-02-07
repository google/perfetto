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
  LegacyTableColumn,
  tableColumnId,
  LegacyTableManager,
} from './column';
import {Button} from '../../../../widgets/button';
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
import {BasicTable, ReorderableColumns} from '../../../../widgets/basic_table';
import {Spinner} from '../../../../widgets/spinner';

import {ArgumentSelector} from './argument_selector';
import {
  LegacySqlTableFilterOptions,
  LegacySqlTableFilterLabel,
} from './render_cell_utils';
import {SqlTableState} from './state';
import {SqlTableDescription} from './table_description';
import {Intent} from '../../../../widgets/common';
import {Form} from '../../../../widgets/form';
import {TextInput} from '../../../../widgets/text_input';

export interface SqlTableConfig {
  readonly state: SqlTableState;
  // For additional menu items to add to the column header menus
  readonly addColumnMenuItems?: (
    column: LegacyTableColumn,
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
  column: LegacyTableColumn,
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

export function columnTitle(column: LegacyTableColumn): string {
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
  filterOption: LegacySqlTableFilterLabel;
  columns: SqlColumn[];
  state: SqlTableState;
  extraAddFilterActions?: (op: string, column: string, value?: string) => void;
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
              state.addFilter({
                op: (cols) => `${cols[0]} ${op}`,
                columns,
              });

              // Extra actions
              attrs.extraAddFilterActions?.(
                filterOption,
                typeof columns[0] === 'string' ? columns[0] : columns[0].column,
              );
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

              // Extra actions
              attrs.extraAddFilterActions?.(
                filterOption,
                typeof columns[0] === 'string' ? columns[0] : columns[0].column,
                this.inputValue,
              );
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

  renderFilters(
    extraRemoveFilterActions?: (filterSqlStr: string) => void,
  ): m.Children {
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

            if (extraRemoveFilterActions) {
              extraRemoveFilterActions(label);
            }
          },
        }),
      );
    }
    return filters;
  }

  renderAddColumnOptions(
    addColumn: (column: LegacyTableColumn) => void,
  ): m.Children {
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
      if (column instanceof LegacyTableColumn) {
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
              onArgumentSelected: (column: LegacyTableColumn) => {
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
    c: LegacyTableColumn,
    extraAddFilterActions?: (
      op: string,
      column: string,
      value?: string,
    ) => void,
  ): m.Vnode<ColumnFilterAttrs, unknown>[] {
    return Object.keys(LegacySqlTableFilterOptions).map((label) =>
      m(ColumnFilter, {
        filterOption: label as LegacySqlTableFilterLabel,
        columns: [c.primaryColumn()],
        state: this.state,
        extraAddFilterActions,
      }),
    );
  }

  renderColumnHeader(
    column: LegacyTableColumn,
    index: number,
    additionalColumnHeaderMenuItems?: m.Children,
    extraAddFilterActions?: (
      op: string,
      column: string,
      value?: string,
    ) => void,
  ) {
    const sorted = this.state.isSortedBy(column);
    const icon =
      sorted === 'ASC'
        ? Icons.SortedAsc
        : sorted === 'DESC'
          ? Icons.SortedDesc
          : Icons.ContextMenu;

    return m(
      PopupMenu,
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
        this.renderColumnFilterOptions(column, extraAddFilterActions),
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
      column: LegacyTableColumn,
      columnAlias: string,
    ) => m.Children,
  ) {
    if (addColumnMenuItems === undefined) return;

    const additionalColumnMenuItems: AdditionalColumnMenuItems = {};
    this.state.getSelectedColumns().forEach((column) => {
      const columnAlias =
        this.state.getCurrentRequest().columns[
          sqlColumnId(column.primaryColumn())
        ];

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
              this.state.getCurrentRequest().columns[
                sqlColumnId(column.primaryColumn())
              ]
            ],
          attrs.extraAddFilterActions,
        ),
        render: (row: Row) => renderCell(column, row, this.state),
      };
    });

    return [
      m('div', this.renderFilters(attrs.extraRemoveFilterActions)),
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

export function getTableManager(state: SqlTableState): LegacyTableManager {
  return {
    addFilter: (filter) => {
      state.addFilter(filter);
    },
    removeFilter: (filter) => {
      state.removeFilter(filter);
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
