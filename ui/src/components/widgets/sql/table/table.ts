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
import {buildSqlQuery} from './query_builder';
import {Icons} from '../../../../base/semantic_icons';
import {Row} from '../../../../trace_processor/query_result';

import {SqlTableState} from './state';
import {SqlTableDescription} from './table_description';
import {
  RenderedCell,
  TableColumn,
  TableManager,
  tableColumnId,
  tableColumnAlias,
} from './table_column';
import {SqlColumn, sqlColumnId} from './sql_column';
import {SelectColumnMenu} from './menus/select_column_menu';
import {renderCastColumnMenu} from './menus/cast_column_menu';
import {renderTransformColumnMenu} from './menus/transform_column_menu';
import {DataGrid} from '../../data_grid/data_grid';
import {
  ColumnDefinition,
  DataGridFilter,
  Sorting,
} from '../../data_grid/common';
import {SqlTableDataSource} from './sql_table_data_source';
import {Filter} from './filters';
import {sqlValueToSqliteString} from '../../../../trace_processor/sql_utils';
import {isQuantitativeType} from '../../../../trace_processor/perfetto_sql_type';

export interface SqlTableConfig {
  readonly state: SqlTableState;
  // For additional menu items to add to the column header menus
  readonly addColumnMenuItems?: (column: TableColumn) => m.Children;
}

function renderCell(
  column: TableColumn,
  row: Row,
  state: SqlTableState,
): RenderedCell {
  const {columns} = state.getCurrentRequest();
  const sqlValue = row[columns[sqlColumnId(column.display ?? column.column)]];

  const result = column.renderCell(sqlValue, getTableManager(state));

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
  private dataSource: SqlTableDataSource;

  constructor(vnode: m.Vnode<SqlTableConfig>) {
    this.state = vnode.attrs.state;
    this.table = this.state.config;
    this.dataSource = new SqlTableDataSource(this.state);
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
        cellMenuItems: (_value, row) => {
          const {menu} = renderCell(column, row as Row, this.state);
          return menu;
        },
      };
      return columnDef;
    });
  }

  private sqlFilterToDataGridFilter(
    sqlFilter: Filter,
  ): DataGridFilter | undefined {
    // Try to convert SqlTable Filter back to DataGridFilter
    // This is best-effort - some complex filters may not convert
    if (sqlFilter.columns.length !== 1) {
      // DataGrid filters work on single columns
      return undefined;
    }

    const columnName = sqlColumnId(sqlFilter.columns[0]);
    const sqlExp = sqlFilter.op([columnName]);

    // Parse common filter patterns
    if (sqlExp === `${columnName} IS NULL`) {
      return {column: columnName, op: 'is null'};
    }
    if (sqlExp === `${columnName} IS NOT NULL`) {
      return {column: columnName, op: 'is not null'};
    }

    // Try to match basic operators with values
    const match = sqlExp.match(
      new RegExp(`^${columnName}\\s+(=|!=|<|<=|>|>=|glob|not glob)\\s+(.+)$`),
    );
    if (match) {
      const op = match[1];
      const value = match[2];

      // Return properly typed filter based on operator
      if (
        op === '=' ||
        op === '!=' ||
        op === '<' ||
        op === '<=' ||
        op === '>' ||
        op === '>=' ||
        op === 'glob' ||
        op === 'not glob'
      ) {
        return {column: columnName, op, value};
      }
    }

    // Can't convert this filter
    return undefined;
  }

  private convertSortingToDataGrid(): Sorting {
    const orderBy = this.state.getOrderedBy();
    if (orderBy.length === 0) {
      return {direction: 'UNSORTED'};
    }
    const firstOrder = orderBy[0];
    // Find the column to get its alias
    const columns = this.state.getSelectedColumns();
    const column = columns.find(
      (c) => tableColumnId(c) === sqlColumnId(firstOrder.column),
    );
    return {
      column: column
        ? tableColumnAlias(column)
        : sqlColumnId(firstOrder.column),
      direction: firstOrder.direction,
    };
  }

  private dataGridFilterToSqlFilter(
    dgFilter: DataGridFilter,
    column: TableColumn,
  ): Filter {
    if ('value' in dgFilter) {
      if (Array.isArray(dgFilter.value)) {
        // Handle 'in' and 'not in' operators
        const values = dgFilter.value.map(sqlValueToSqliteString).join(', ');
        return {
          op: (cols) =>
            dgFilter.op === 'in'
              ? `${cols[0]} IN (${values})`
              : `${cols[0]} NOT IN (${values})`,
          columns: [column.column],
        };
      } else {
        // Handle operators with single values
        const value = sqlValueToSqliteString(dgFilter.value);
        return {
          op: (cols) => `${cols[0]} ${dgFilter.op} ${value}`,
          columns: [column.column],
        };
      }
    } else {
      // Handle 'is null' and 'is not null'
      return {
        op: (cols) => `${cols[0]} ${dgFilter.op.toUpperCase()}`,
        columns: [column.column],
      };
    }
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

      const {content} = renderCell(column, row as Row, this.state);
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
      sorting: this.convertSortingToDataGrid(),
      onSort: (sorting: Sorting) => {
        if (sorting.direction === 'UNSORTED') {
          // Clear sorting
          if (columns.length > 0) {
            this.state.sortBy({column: columns[0], direction: undefined});
          }
        } else {
          // sorting.column is the aliased name
          const column = columns.find(
            (c) => tableColumnAlias(c) === sorting.column,
          );
          if (column) {
            this.state.sortBy({column, direction: sorting.direction});
          }
        }
      },
      // Use DataGrid's filter system and sync to SqlTable state
      filters: this.state.filters
        .get()
        .map((f) => this.sqlFilterToDataGridFilter(f))
        .filter((f): f is DataGridFilter => f !== undefined),
      onFilterAdd: (dgFilter: DataGridFilter) => {
        // dgFilter.column is the aliased name
        const column = columns.find(
          (c) => tableColumnAlias(c) === dgFilter.column,
        );
        if (column) {
          const sqlFilter = this.dataGridFilterToSqlFilter(dgFilter, column);
          this.state.filters.addFilter(sqlFilter);
        }
      },
      onFilterRemove: (index: number) => {
        const currentFilters = this.state.filters.get();
        if (index >= 0 && index < currentFilters.length) {
          this.state.filters.removeFilter(currentFilters[index]);
        }
      },
      clearFilters: () => {
        this.state.filters.clear();
      },
      cellRenderer,
      fillHeight: true,
      className: 'sql-table',
      columnReordering: true,
      showFiltersInToolbar: false,
    });
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
