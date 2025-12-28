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
import {PivotTableState} from './pivot_table_state';
import {Spinner} from '../../../../widgets/spinner';
import {PivotTreeNode} from './pivot_tree_node';
import {Button} from '../../../../widgets/button';
import {Icons} from '../../../../base/semantic_icons';
import {TableColumn, tableColumnId} from '../table/table_column';
import {MenuDivider, MenuItem} from '../../../../widgets/menu';
import {SelectColumnMenu} from '../table/menus/select_column_menu';
import {SqlColumn} from '../table/sql_column';
import {buildSqlQuery} from '../table/query_builder';
import {Aggregation, AGGREGATIONS} from './aggregations';
import {aggregationId, pivotId} from './ids';
import {
  Grid,
  GridCell,
  GridColumn,
  GridHeaderCell,
  renderSortMenuItems,
  SortDirection,
} from '../../../../widgets/grid';

export interface PivotTableAttrs {
  readonly state: PivotTableState;
  readonly getSelectableColumns: () => TableColumn[];
  // Additional button to render at the end of each row. Typically used
  // for adding new filters.
  extraRowButton?(node: PivotTreeNode): m.Children;
}

export class PivotTable implements m.ClassComponent<PivotTableAttrs> {
  view({attrs}: m.CVnode<PivotTableAttrs>) {
    const state = attrs.state;
    const data = state.getData();
    const pivots = state.getPivots();
    const aggregations = state.getAggregations();
    const extraRowButton = attrs.extraRowButton;

    // Expand the tree to a list of rows to show
    const nodes: PivotTreeNode[] = data ? [...data.listDescendants()] : [];

    // Build VirtualGrid columns
    const columns: GridColumn[] = [
      ...pivots.map((pivot, index) => {
        const sorted = state.isSortedByPivot(pivot);
        const columnKey = `pivot-${pivotId(pivot)}`;
        const gridColumn: GridColumn = {
          key: columnKey,
          header: m(
            GridHeaderCell,
            {
              sort: sorted,
              onSort: (direction: SortDirection) =>
                state.sortByPivot(pivot, direction),
              menuItems: this.renderPivotColumnMenu(attrs, pivot, index),
            },
            pivotId(pivot),
          ),
          reorderable: {reorderGroup: 'pivot'},
          thickRightBorder: index === pivots.length - 1,
        };
        return gridColumn;
      }),
      ...aggregations.map((agg, index) => {
        const columnKey = `agg-${aggregationId(agg)}`;
        const gridColumn: GridColumn = {
          key: columnKey,
          header: m(
            GridHeaderCell,
            {
              sort: state.isSortedByAggregation(agg),
              onSort: (direction: SortDirection) =>
                state.sortByAggregation(agg, direction),
              menuItems: this.renderAggregationColumnMenu(attrs, agg, index),
            },
            aggregationId(agg),
          ),
          reorderable: {reorderGroup: 'aggregation'},
        };
        return gridColumn;
      }),
    ];

    if (extraRowButton) {
      columns.push({
        key: 'action-button',
        widthPx: 24,
        header: m(GridHeaderCell, ''),
      });
    }

    // Build VirtualGrid rows
    const rows = nodes.map((node) => {
      const cellRow: m.Children[] = [];

      // Handle pivot cells
      if (node.isRoot()) {
        // For root node, create a special "Total values" cell that spans all pivot columns
        // We'll just put it in the first pivot column and leave others empty
        cellRow.push(
          m(
            GridCell,
            {
              align: 'right',
            },
            m('.pf-pivot-table__total-values', 'Total values:'),
          ),
        );

        // Leave other pivot columns empty for the root row
        for (let i = 1; i < pivots.length; i++) {
          cellRow.push(m(GridCell));
        }
      } else {
        // Regular pivot cells
        pivots.forEach((_, index) => {
          const status = node.getPivotDisplayStatus(index);
          const value = node.getPivotValue(index);
          const renderedCell = (function () {
            if (value === undefined) return undefined;
            return state.getPivots()[index].renderCell(value);
          })();
          const content = [
            (status === 'collapsed' || status === 'expanded') &&
              m(Button, {
                icon:
                  status === 'collapsed' ? 'chevron_right' : Icons.ExpandDown,
                onclick: () => {
                  node.collapsed = !node.collapsed;
                  m.redraw();
                },
                compact: true,
              }),
            status === 'auto_expanded' &&
              m(Button, {
                icon: 'chevron_right',
                disabled: true,
                compact: true,
              }),
            status === 'pivoted_value' &&
              m('span.pf-pivot-table__cell--indent'),
            renderedCell && renderedCell.content,
            status === 'hidden_behind_collapsed' && '...',
          ];
          cellRow.push(
            m(
              GridCell,
              {
                align: renderedCell?.isNull
                  ? 'center'
                  : renderedCell?.isNumerical
                    ? 'right'
                    : 'left',
                nullish: renderedCell?.isNull,
              },
              content,
            ),
          );
        });
      }

      // Handle aggregation cells
      aggregations.forEach((agg, index) => {
        const renderedCell = agg.column.renderCell(
          node.getAggregationValue(index),
        );
        cellRow.push(
          m(
            GridCell,
            {
              align: renderedCell?.isNull
                ? 'center'
                : renderedCell?.isNumerical
                  ? 'right'
                  : 'left',
              nullish: renderedCell?.isNull,
            },
            renderedCell.content,
          ),
        );
      });

      // Handle extra row button
      if (extraRowButton) {
        cellRow.push(m(GridCell, {padding: false}, extraRowButton(node)));
      }

      return cellRow;
    });

    return [
      m(Grid, {
        fillHeight: true,
        className: 'pf-pivot-table',
        columns,
        rowData: rows,
        virtualization: {
          rowHeightPx: 25,
        },
        onColumnReorder: (from, to, position) => {
          if (typeof from === 'string' && typeof to === 'string') {
            // Handle pivot column reordering
            if (from.startsWith('pivot-') && to.startsWith('pivot-')) {
              const fromIndex = pivots.findIndex(
                (p) => `pivot-${pivotId(p)}` === from,
              );
              let toIndex = pivots.findIndex(
                (p) => `pivot-${pivotId(p)}` === to,
              );
              if (position === 'after') {
                toIndex++;
              }
              state.movePivot(fromIndex, toIndex);
            }
            // Handle aggregation column reordering
            else if (from.startsWith('agg-') && to.startsWith('agg-')) {
              const fromIndex = aggregations.findIndex(
                (a) => `agg-${aggregationId(a)}` === from,
              );
              let toIndex = aggregations.findIndex(
                (a) => `agg-${aggregationId(a)}` === to,
              );
              if (position === 'after') {
                toIndex++;
              }
              state.moveAggregation(fromIndex, toIndex);
            }
          }
        },
      }),
      data === undefined && m(Spinner),
    ];
  }

  renderPivotColumnMenu(
    attrs: PivotTableAttrs,
    pivot: TableColumn,
    index: number,
  ): m.Children {
    const state = attrs.state;
    const sorted = state.isSortedByPivot(pivot);
    const menuItems: m.Children = [];

    menuItems.push(
      // Sort by pivot.
      renderSortMenuItems(sorted, (direction) =>
        state.sortByPivot(pivot, direction),
      ),

      m(MenuDivider),

      m(
        MenuItem,
        {
          label: 'Add pivot',
          icon: Icons.Add,
        },
        m(SelectColumnMenu, {
          columns: attrs.getSelectableColumns().map((column) => ({
            key: tableColumnId(column),
            column,
          })),
          filters: state.filters,
          trace: state.trace,
          getSqlQuery: (columns: {[key: string]: SqlColumn}) =>
            buildSqlQuery({
              table: state.table.name,
              columns,
              filters: state.filters.get(),
            }),
          existingColumnIds: new Set(state.getPivots().map(pivotId)),
          onColumnSelected: (column) => state.addPivot(column, index),
        }),
      ),

      m(MenuDivider),

      // Remove pivot: show only if there is more than one pivot (to avoid
      // removing the last pivot).
      m(MenuItem, {
        disabled: state.getPivots().length === 1,
        label: 'Remove',
        icon: Icons.Delete,
        onclick: () => state.removePivot(index),
      }),
    );
    return menuItems;
  }

  renderAggregationColumnMenu(
    attrs: PivotTableAttrs,
    agg: Aggregation,
    index: number,
  ): m.Children {
    const state = attrs.state;
    const sorted = state.isSortedByAggregation(agg);
    const menuItems: m.Children = [];

    menuItems.push(
      // Sort by aggregation.
      renderSortMenuItems(sorted, (direction) =>
        state.sortByAggregation(agg, direction),
      ),

      // Change aggregation operation, add the same aggregation again, and remove
      // aggregation are not available for the count aggregation.
      agg.op !== 'count' && [
        m(MenuDivider),
        m(
          MenuItem,
          {
            label: 'Change aggregation',
            icon: Icons.Change,
          },
          AGGREGATIONS.filter((a) => a !== agg.op).map((a) =>
            m(MenuItem, {
              label: a,
              onclick: () =>
                state.replaceAggregation(index, {
                  op: a,
                  column: agg.column,
                }),
            }),
          ),
        ),

        // Add the same aggregation again.
        // Designed to be used together with "change aggregation" to allow the user to add multiple
        // aggregations on the same column (e.g. MIN / MAX).
        m(MenuItem, {
          label: 'Duplicate',
          icon: Icons.Copy,
          onclick: () => state.addAggregation(agg, index + 1),
        }),
        m(MenuItem, {
          label: 'Remove',
          icon: Icons.Delete,
          onclick: () => state.removeAggregation(index),
        }),
      ],

      // End of "per-pivot" menu items. The following menu items are table-level
      // operations (i.e. "add pivot").
      m(MenuDivider),

      m(
        MenuItem,
        {
          label: 'Add aggregation',
          icon: Icons.Add,
        },
        m(SelectColumnMenu, {
          columns: attrs.getSelectableColumns().map((column) => ({
            key: tableColumnId(column),
            column,
          })),
          filters: state.filters,
          trace: state.trace,
          getSqlQuery: (columns: {[key: string]: SqlColumn}) =>
            buildSqlQuery({
              table: state.table.name,
              columns,
              filters: state.filters.get(),
            }),
          columnMenu: (column) => ({
            rightIcon: Icons.ContextMenuAlt,
            children: AGGREGATIONS.map((agg) =>
              m(MenuItem, {
                label: agg,
                onclick: () => state.addAggregation({op: agg, column}, index),
              }),
            ),
          }),
        }),
      ),
    );
    return menuItems;
  }
}
