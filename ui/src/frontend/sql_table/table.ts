// Copyright (C) 2023 The Android Open Source Project
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

import {isString} from '../../base/object_utils';
import {Icons} from '../../base/semantic_icons';
import {EngineProxy} from '../../common/engine';
import {Row} from '../../common/query_result';
import {Anchor} from '../../widgets/anchor';
import {BasicTable} from '../../widgets/basic_table';
import {Button} from '../../widgets/button';
import {MenuDivider, MenuItem, PopupMenu2} from '../../widgets/menu';
import {Spinner} from '../../widgets/spinner';

import {ArgumentSelector} from './argument_selector';
import {argColumn, Column, columnFromSqlTableColumn} from './column';
import {renderCell} from './render_cell';
import {SqlTableState} from './state';
import {isArgSetIdColumn, SqlTableDescription} from './table_description';

export interface SqlTableConfig {
  readonly state: SqlTableState;
}

export class SqlTable implements m.ClassComponent<SqlTableConfig> {
  private readonly table: SqlTableDescription;
  private readonly engine: EngineProxy;

  private state: SqlTableState;

  constructor(vnode: m.Vnode<SqlTableConfig>) {
    this.state = vnode.attrs.state;
    this.table = this.state.table;
    this.engine = this.state.engine;
  }

  renderFilters(): m.Children {
    const filters: m.Child[] = [];
    for (const filter of this.state.getFilters()) {
      const label =
          isString(filter) ? filter : `Arg(${filter.argName}) ${filter.op}`;
      filters.push(m(Button, {
        label,
        icon: 'close',
        onclick: () => {
          this.state.removeFilter(filter);
        },
      }));
    }
    return filters;
  }

  renderAddColumnOptions(addColumn: (column: Column) => void): m.Children {
    // We do not want to add columns which already exist, so we track the
    // columns which we are already showing here.
    // TODO(altimin): Theoretically a single table can have two different
    // arg_set_ids, so we should track (arg_set_id_column, arg_name) pairs here.
    const existingColumns = new Set<string>();

    for (const column of this.state.getSelectedColumns()) {
      existingColumns.add(column.alias);
    }

    const result = [];
    for (const column of this.table.columns) {
      if (existingColumns.has(column.name)) continue;
      if (isArgSetIdColumn(column)) {
        result.push(
            m(MenuItem,
              {
                label: column.name,
              },
              m(ArgumentSelector, {
                engine: this.engine,
                argSetId: column,
                tableName: this.table.name,
                constraints: this.state.getQueryConstraints(),
                alreadySelectedColumns: existingColumns,
                onArgumentSelected: (argument: string) => {
                  addColumn(argColumn(column, argument));
                },
              })));
        continue;
      }
      result.push(m(MenuItem, {
        label: column.name,
        onclick: () => addColumn(
            columnFromSqlTableColumn(column),
            ),
      }));
    }
    return result;
  }

  renderColumnHeader(column: Column, index: number) {
    const sorted = this.state.isSortedBy(column);
    const icon = sorted === 'ASC' ?
        Icons.SortedAsc :
        sorted === 'DESC' ? Icons.SortedDesc : Icons.ContextMenu;
    return m(
        PopupMenu2,
        {
          trigger: m(Anchor, {icon}, column.title),
        },
        sorted !== 'DESC' && m(MenuItem, {
          label: 'Sort: highest first',
          icon: Icons.SortedDesc,
          onclick: () => {
            this.state.sortBy({column, direction: 'DESC'});
          },
        }),
        sorted !== 'ASC' && m(MenuItem, {
          label: 'Sort: lowest first',
          icon: Icons.SortedAsc,
          onclick: () => {
            this.state.sortBy({column, direction: 'ASC'});
          },
        }),
        sorted !== undefined && m(MenuItem, {
          label: 'Unsort',
          icon: Icons.Close,
          onclick: () => this.state.unsort(),
        }),
        this.state.getSelectedColumns().length > 1 && m(MenuItem, {
          label: 'Hide',
          icon: Icons.Hide,
          onclick: () => this.state.hideColumnAtIndex(index),
        }),
        m(MenuDivider),
        m(MenuItem,
          {label: 'Add column', icon: Icons.AddColumn},
          this.renderAddColumnOptions((column) => {
            this.state.addColumn(column, index);
          })),
    );
  }

  view() {
    const rows = this.state.getDisplayedRows();

    return [
      m('div', this.renderFilters()),
      m(BasicTable, {
        data: rows,
        columns: this.state.getSelectedColumns().map(
            (column, i) => ({
              title: this.renderColumnHeader(column, i),
              render: (row: Row) => renderCell(column, row, this.state),
            })),
      }),
      this.state.isLoading() && m(Spinner),
      this.state.getQueryError() !== undefined &&
          m('.query-error', this.state.getQueryError()),
    ];
  }
};

export {SqlTableDescription};
