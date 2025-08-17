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
import {SelectColumnMenu} from '../table/select_column_menu';
import {SqlColumn} from '../table/sql_column';
import {buildSqlQuery} from '../table/query_builder';
import {Aggregation, AGGREGATIONS} from './aggregations';
import {aggregationId, pivotId} from './ids';
import {
  Grid,
  GridBody,
  GridDataCell,
  GridHeader,
  GridHeaderCell,
  GridRow,
  renderSortMenuItems,
} from '../../../../widgets/grid';

export interface PivotTableAttrs {
  readonly state: PivotTableState;
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

    const headers = [
      ...pivots.map((pivot, index) => {
        const sorted = state.isSortedByPivot(pivot);
        return m(
          GridHeaderCell,
          {
            key: `pivot-${pivotId(pivot)}`,
            reorderable: {handle: 'pivot'},
            onReorder: (from, to, position) => {
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
            },
            sort: sorted,
            onSort: (direction) => state.sortByPivot(pivot, direction),
            menuItems: this.renderPivotColumnMenu(attrs, pivot, index),
            thickRightBorder: index === pivots.length - 1,
          },
          pivotId(pivot),
        );
      }),
      ...aggregations.map((agg, index) => {
        return m(
          GridHeaderCell,
          {
            key: `agg-${aggregationId(agg)}`,
            reorderable: {handle: 'aggregation'},
            onReorder: (from, to, position) => {
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
            },
            sort: state.isSortedByAggregation(agg),
            onSort: (direction) => state.sortByAggregation(agg, direction),
            menuItems: this.renderAggregationColumnMenu(attrs, agg, index),
          },
          aggregationId(agg),
        );
      }),
    ];

    if (extraRowButton) {
      headers.push(m(GridHeaderCell, {key: 'action-button'}));
    }

    // Expand the tree to a list of rows to show.
    const nodes: PivotTreeNode[] = data ? [...data.listDescendants()] : [];

    return [
      m(
        Grid,
        {
          fillHeight: true,
          className: 'pf-pivot-table',
        },
        [
          m(GridHeader, m(GridRow, headers)),
          m(
            GridBody,
            nodes.map((node) => {
              const pivotCells = node.isRoot()
                ? [
                    m(
                      GridDataCell,
                      {
                        align: 'right',
                        colspan: pivots.length,
                        thickRightBorder: true,
                      },
                      m('.pf-pivot-table__total-values', 'Total values:'),
                    ),
                  ]
                : pivots.map((_pivot, index) => {
                    const status = node.getPivotDisplayStatus(index);
                    const value = node.getPivotValue(index);
                    const content = [
                      (status === 'collapsed' || status === 'expanded') &&
                        m(Button, {
                          icon:
                            status === 'collapsed'
                              ? 'chevron_right'
                              : Icons.ExpandDown,
                          onclick: () => {
                            node.collapsed = !node.collapsed;
                            m.redraw();
                          },
                          compact: true,
                        }),
                      // Show a non-clickable indicator that the value is auto-expanded.
                      status === 'auto_expanded' &&
                        m(Button, {
                          icon: 'chevron_right',
                          disabled: true,
                          compact: true,
                        }),
                      // Indent the expanded values to align them with the parent value
                      // even though they do not have the "expand/collapse" button.
                      status === 'pivoted_value' &&
                        m('span.pf-pivot-table__cell--indent'),
                      value !== undefined &&
                        state.getPivots()[index].renderCell(value).content,
                      // Show ellipsis for the last pivot if the node is collapsed to
                      // make it clear to the user that there are some values.
                      status === 'hidden_behind_collapsed' && '...',
                    ];
                    return m(
                      GridDataCell,
                      {thickRightBorder: index === pivots.length - 1},
                      content,
                    );
                  });

              const aggregationCells = aggregations.map((agg, index) => {
                const content = agg.column.renderCell(
                  node.getAggregationValue(index),
                ).content;
                return m(GridDataCell, content);
              });

              const cells = [...pivotCells, ...aggregationCells];

              if (extraRowButton) {
                cells.push(
                  m(
                    GridDataCell,
                    {className: 'action-button'},
                    extraRowButton(node),
                  ),
                );
              }

              return m(GridRow, cells);
            }),
          ),
        ],
      ),
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
          columns: state.table.columns.map((column) => ({
            key: tableColumnId(column),
            column,
          })),
          manager: {
            filters: state.filters,
            trace: state.trace,
            getSqlQuery: (columns: {[key: string]: SqlColumn}) =>
              buildSqlQuery({
                table: state.table.name,
                columns,
                filters: state.filters.get(),
              }),
          },
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
          columns: state.table.columns.map((column) => ({
            key: tableColumnId(column),
            column,
          })),
          manager: {
            filters: state.filters,
            trace: state.trace,
            getSqlQuery: (columns: {[key: string]: SqlColumn}) =>
              buildSqlQuery({
                table: state.table.name,
                columns,
                filters: state.filters.get(),
              }),
          },
          columnMenu: (column) => ({
            rightIcon: '',
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
