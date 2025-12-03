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
import {MenuDivider, MenuItem} from '../../../../widgets/menu';
import {Icons} from '../../../../base/semantic_icons';
import {Row} from '../../../../trace_processor/query_result';

import {SqlTableState} from './state';
import {SqlTableDescription} from './table_description';
import {
  RenderedCell,
  TableColumn,
  tableColumnId,
  tableColumnAlias,
} from './table_column';
import {sqlColumnId} from './sql_column';
import {SelectColumnMenu} from './menus/select_column_menu';
import {renderCastColumnMenu} from './menus/cast_column_menu';
import {renderTransformColumnMenu} from './menus/transform_column_menu';
import {DataGrid} from '../../data_grid/data_grid';
import {ColumnDefinition} from '../../data_grid/common';
import {SQLDataSource} from '../../data_grid/sql_data_source';
import {isQuantitativeType} from '../../../../trace_processor/perfetto_sql_type';

export interface SqlTableConfig {
  readonly state: SqlTableState;
  // For additional menu items to add to the column header menus
  readonly addColumnMenuItems?: (column: TableColumn) => m.Children;
}

function renderCell(
  column: TableColumn,
  row: Row,
  columnAliasMap: {[key: string]: string},
): RenderedCell {
  const alias = columnAliasMap[sqlColumnId(column.display ?? column.column)];
  const sqlValue = row[alias];

  // TableManager is no longer needed since filters/queries are managed elsewhere
  // Pass undefined for now - columns should not depend on TableManager
  const result = column.renderCell(sqlValue, undefined!);

  return result;
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

export class SqlTable implements m.ClassComponent<SqlTableConfig> {
  private readonly table: SqlTableDescription;
  private state: SqlTableState;
  private dataSource: SQLDataSource;
  private columnAliasMap: {[key: string]: string} = {};

  constructor(vnode: m.Vnode<SqlTableConfig>) {
    this.state = vnode.attrs.state;
    this.table = this.state.config;
    this.dataSource = this.createDataSource();

    // Recreate datasource when columns change
    this.state.onColumnsChanged(() => {
      this.dataSource = this.createDataSource();
      m.redraw();
    });
  }

  private createDataSource(): SQLDataSource {
    const baseQuery = this.state.buildBaseQuery();
    const imports = this.state.getSQLImports();
    this.columnAliasMap = this.state.getColumnAliasMap();
    return new SQLDataSource(this.state.trace.engine, baseQuery, imports);
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
      manager: undefined!, // TableManager no longer needed
      existingColumnIds,
      onColumnSelected: addColumn,
    });
  }

  private convertTableColumnsToDataGridColumns(
    columns: readonly TableColumn[],
    addColumnMenuItems?: (column: TableColumn) => m.Children,
  ): ColumnDefinition[] {
    return columns.map((column, i) => {
      // Use the aliased name (with escaped characters) to match the SQL result keys
      const aliasedName = tableColumnAlias(column);

      // Determine filter type based on column's SQL type
      const filterType = column.type
        ? isQuantitativeType(column.type)
          ? 'numeric'
          : 'string'
        : undefined;

      const columnDef: ColumnDefinition = {
        distinctValues: false, // Distinct values are not supported for SqlTable columns
        name: aliasedName,
        title: columnTitle(column),
        filterType,
        contextMenuRenderer: (builtins) => {
          // Column-specific menu items from the column itself
          const columnSpecificItems = column.getColumnSpecificMenuItems?.({
            replaceColumn: (newColumn: TableColumn) =>
              this.state.replaceColumnAtIndex(i, newColumn),
          });

          return [
            builtins.sorting,
            m(MenuDivider),
            m(MenuItem, {
              label: 'Remove column',
              icon: Icons.Delete,
              onclick: () => this.state.hideColumnAtIndex(i),
            }),
            builtins.fitToContent,
            m(MenuDivider),
            builtins.filters,
            columnSpecificItems,
            m(
              MenuItem,
              {label: 'Cast', icon: Icons.Change},
              renderCastColumnMenu(column, i, this.state),
            ),
            renderTransformColumnMenu(column, i, this.state),
            addColumnMenuItems && addColumnMenuItems(column),
            m(MenuDivider),
            m(AddColumnMenuItem, {
              table: this,
              state: this.state,
              index: i,
            }),
          ];
        },
        cellContextMenuRenderer: (_value, row, builtins) => {
          // Get the menu from renderCell to allow column-specific context menus
          const {menu} = renderCell(column, row as Row, this.columnAliasMap);
          return [menu, builtins.addFilter];
        },
      };
      return columnDef;
    });
  }

  view({attrs}: m.Vnode<SqlTableConfig>) {
    const columns = this.state.getSelectedColumns();
    const dataGridColumns = this.convertTableColumnsToDataGridColumns(
      columns,
      attrs.addColumnMenuItems,
    );

    // Convert column order - use aliases to match DataGrid column names
    const columnOrder = columns.map((c) => tableColumnAlias(c));

    // Custom cell renderer that uses TableColumn's renderCell
    const cellRenderer = (value: unknown, columnName: string, row: unknown) => {
      // columnName is the aliased name, so compare with aliases
      const column = columns.find((c) => tableColumnAlias(c) === columnName);
      if (!column) return String(value);

      const {content} = renderCell(column, row as Row, this.columnAliasMap);
      return content;
    };

    return m(DataGrid, {
      columns: dataGridColumns,
      data: this.dataSource,
      columnOrder,
      onColumnOrderChanged: (newOrder: ReadonlyArray<string>) => {
        // Handle column reordering - find which column moved and where
        for (let i = 0; i < newOrder.length; i++) {
          if (newOrder[i] !== columnOrder[i]) {
            const movedColumn = newOrder[i];
            const fromIndex = columnOrder.indexOf(movedColumn);
            const toIndex = i;
            if (fromIndex !== -1 && fromIndex !== toIndex) {
              this.state.moveColumn(fromIndex, toIndex);
            }
            return;
          }
        }
      },
      cellRenderer,
      fillHeight: true,
      className: 'sql-table',
      columnReordering: true,
      showFiltersInToolbar: false,
    });
  }
}
