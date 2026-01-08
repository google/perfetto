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
import {Spinner} from '../../../../widgets/spinner';
import {
  Grid,
  GridCell,
  GridColumn,
  GridHeaderCell,
  renderSortMenuItems,
  SortDirection,
} from '../../../../widgets/grid';

import {SqlTableState} from './state';
import {SqlTableDescription} from './table_description';
import {
  RenderedCell,
  TableColumn,
  RenderCellContext,
  tableColumnId,
} from './table_column';
import {SqlColumn, sqlColumnId} from './sql_column';
import {SelectColumnMenu} from './menus/select_column_menu';
import {renderColumnFilterOptions} from './menus/add_column_filter_menu';
import {renderCastColumnMenu} from './menus/cast_column_menu';
import {renderTransformColumnMenu} from './menus/transform_column_menu';

export interface SqlTableConfig {
  readonly state: SqlTableState;
  // For additional menu items to add to the column header menus
  readonly addColumnMenuItems?: (column: TableColumn) => m.Children;
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
  addColumn: (column: TableColumn) => void,
): RenderedCell {
  const {columns} = state.getCurrentRequest();
  const sqlValue = row[columns[sqlColumnId(column.display ?? column.column)]];

  const result = column.renderCell(
    sqlValue,
    getRenderCellContext(state, addColumn),
  );

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
      filters: this.state.filters,
      trace: this.state.trace,
      getSqlQuery: (columns: {[key: string]: SqlColumn}) =>
        buildSqlQuery({
          table: this.state.config.name,
          columns,
          filters: this.state.filters.get(),
          orderBy: this.state.getOrderedBy(),
        }),
      existingColumnIds,
      onColumnSelected: addColumn,
    });
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

    // Build VirtualGrid columns
    const virtualGridColumns = columns.map((column, i) => {
      const sorted = this.state.isSortedBy(column);
      const menuItems: m.Children = [
        renderSortMenuItems(sorted, (direction) =>
          this.state.sortBy({column, direction}),
        ),
        m(MenuDivider),
        this.state.getSelectedColumns().length > 1 &&
          m(MenuItem, {
            label: 'Hide',
            icon: Icons.Hide,
            onclick: () => this.state.hideColumnAtIndex(i),
          }),
        // Use the new getColumnSpecificMenuItems method if available
        column.getColumnSpecificMenuItems?.({
          replaceColumn: (newColumn: TableColumn) =>
            this.state.replaceColumnAtIndex(i, newColumn),
        }),
        m(
          MenuItem,
          {label: 'Cast', icon: Icons.Change},
          renderCastColumnMenu(column, i, this.state),
        ),
        renderTransformColumnMenu(column, i, this.state),
        m(
          MenuItem,
          {label: 'Add filter', icon: Icons.Filter},
          renderColumnFilterOptions(column, this.state),
        ),
        additionalColumnMenuItems &&
          additionalColumnMenuItems[
            this.state.getCurrentRequest().columns[sqlColumnId(column.column)]
          ],
        // Menu items before divider apply to selected column
        m(MenuDivider),
        // Menu items after divider apply to entire table
        m(AddColumnMenuItem, {
          table: this,
          state: this.state,
          index: i,
        }),
      ];
      const columnKey = tableColumnId(column);

      const gridColumn: GridColumn = {
        key: columnKey,
        header: m(
          GridHeaderCell,
          {
            sort: sorted,
            onSort: (direction: SortDirection) => {
              this.state.sortBy({column, direction});
            },
            menuItems,
          },
          columnTitle(column),
        ),
        reorderable: {reorderGroup: 'column'},
      };

      return gridColumn;
    });

    // Build VirtualGrid rows
    const virtualGridRows = rows.map((row) => {
      return columns.map((col, i) => {
        const {content, menu, isNumerical, isNull} = renderCell(
          col,
          row,
          this.state,
          (column) => {
            this.state.addColumn(column, i);
          },
        );
        return m(
          GridCell,
          {
            menuItems: menu,
            align: isNull ? 'center' : isNumerical ? 'right' : 'left',
            nullish: isNull,
          },
          content,
        );
      });
    });

    return [
      m(Grid, {
        className: 'sql-table',
        columns: virtualGridColumns,
        rowData: virtualGridRows,
        fillHeight: true,
        onColumnReorder: (from, to, position) => {
          if (typeof from === 'string' && typeof to === 'string') {
            // Convert column names to indices
            const fromIndex = columns.findIndex(
              (col) => tableColumnId(col) === from,
            );
            const toIndex = columns.findIndex(
              (col) => tableColumnId(col) === to,
            );

            if (fromIndex !== -1 && toIndex !== -1) {
              const targetIndex = position === 'before' ? toIndex : toIndex + 1;
              this.state.moveColumn(fromIndex, targetIndex);
            }
          }
        },
      }),
      this.state.isLoading() && m(Spinner),
      this.state.getQueryError() !== undefined &&
        m('.query-error', this.state.getQueryError()),
    ];
  }
}

function getRenderCellContext(
  state: SqlTableState,
  addColumn: (column: TableColumn) => void,
): RenderCellContext {
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
    hasColumn: (column: TableColumn) => {
      const selectedColumns = state.getSelectedColumns();
      return !selectedColumns.some(
        (c) => tableColumnId(c) === tableColumnId(column),
      );
    },
    addColumn,
  };
}
