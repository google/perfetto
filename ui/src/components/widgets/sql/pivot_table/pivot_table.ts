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
  private columnWidths: Map<string, number> = new Map();
  private hasCalculatedInitialWidths = false;

  view({attrs}: m.CVnode<PivotTableAttrs>) {
    const state = attrs.state;
    const data = state.getData();
    const pivots = state.getPivots();
    const aggregations = state.getAggregations();
    const extraRowButton = attrs.extraRowButton;

    const headers = [
      ...pivots.map((pivot, index) => {
        const sorted = state.isSortedByPivot(pivot);
        const columnKey = `pivot-${pivotId(pivot)}`;
        const width = this.columnWidths.get(columnKey) ?? 100;
        return m(
          GridHeaderCell,
          {
            key: columnKey,
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
            width,
            onResize: (newWidth: number) => {
              this.columnWidths.set(columnKey, newWidth);
              m.redraw();
            },
            onAutoResize: () => {
              const optimalWidth = this.calculateOptimalColumnWidth(
                columnKey,
                pivotId(pivot),
                nodes,
                (node) => {
                  if (node.isRoot()) return undefined;
                  const value = node.getPivotValue(index);
                  if (value === undefined) return undefined;
                  return pivot.renderCell(value);
                },
              );
              this.columnWidths.set(columnKey, optimalWidth);
              m.redraw();
            },
          },
          pivotId(pivot),
        );
      }),
      ...aggregations.map((agg, index) => {
        const columnKey = `agg-${aggregationId(agg)}`;
        const width = this.columnWidths.get(columnKey) ?? 100;
        return m(
          GridHeaderCell,
          {
            key: columnKey,
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
            width,
            onResize: (newWidth: number) => {
              this.columnWidths.set(columnKey, newWidth);
              m.redraw();
            },
            onAutoResize: () => {
              const optimalWidth = this.calculateOptimalColumnWidth(
                columnKey,
                aggregationId(agg),
                nodes,
                (node) => agg.column.renderCell(node.getAggregationValue(index)),
              );
              this.columnWidths.set(columnKey, optimalWidth);
              m.redraw();
            },
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

    // Calculate initial column widths on first render with data
    if (!this.hasCalculatedInitialWidths && nodes.length > 0) {
      pivots.forEach((pivot, index) => {
        const columnKey = `pivot-${pivotId(pivot)}`;
        const optimalWidth = this.calculateOptimalColumnWidth(
          columnKey,
          pivotId(pivot),
          nodes,
          (node) => {
            if (node.isRoot()) return undefined;
            const value = node.getPivotValue(index);
            if (value === undefined) return undefined;
            return pivot.renderCell(value);
          },
        );
        this.columnWidths.set(columnKey, optimalWidth);
      });

      aggregations.forEach((agg, index) => {
        const columnKey = `agg-${aggregationId(agg)}`;
        const optimalWidth = this.calculateOptimalColumnWidth(
          columnKey,
          aggregationId(agg),
          nodes,
          (node) => agg.column.renderCell(node.getAggregationValue(index)),
        );
        this.columnWidths.set(columnKey, optimalWidth);
      });

      this.hasCalculatedInitialWidths = true;
    }

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
                    const renderedCell = (function () {
                      if (value === undefined) return undefined;
                      return state.getPivots()[index].renderCell(value);
                    })();
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
                      renderedCell && renderedCell.content,
                      // Show ellipsis for the last pivot if the node is collapsed to
                      // make it clear to the user that there are some values.
                      status === 'hidden_behind_collapsed' && '...',
                    ];
                    const columnKey = `pivot-${pivotId(pivots[index])}`;
                    const width = this.columnWidths.get(columnKey) ?? 100;
                    return m(
                      GridDataCell,
                      {
                        thickRightBorder: index === pivots.length - 1,
                        align: renderedCell?.isNull
                          ? 'center'
                          : renderedCell?.isNumerical
                            ? 'right'
                            : 'left',
                        nullish: renderedCell?.isNull,
                        width,
                      },
                      content,
                    );
                  });

              const aggregationCells = aggregations.map((agg, index) => {
                const renderedCell = agg.column.renderCell(
                  node.getAggregationValue(index),
                );
                const columnKey = `agg-${aggregationId(agg)}`;
                const width = this.columnWidths.get(columnKey) ?? 100;
                return m(
                  GridDataCell,
                  {
                    align: renderedCell?.isNull
                      ? 'center'
                      : renderedCell?.isNumerical
                        ? 'right'
                        : 'left',
                    nullish: renderedCell?.isNull,
                    width,
                  },
                  renderedCell.content,
                );
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

  private calculateOptimalColumnWidth(
    columnKey: string,
    headerText: string,
    nodes: PivotTreeNode[],
    getCellContent: (node: PivotTreeNode) => {content: m.Children; isNull?: boolean; isNumerical?: boolean} | undefined,
  ): number {
    const measureContainer = document.createElement('div');
    measureContainer.style.position = 'absolute';
    measureContainer.style.visibility = 'hidden';
    measureContainer.style.pointerEvents = 'none';
    measureContainer.style.top = '-9999px';
    measureContainer.style.left = '-9999px';
    document.body.appendChild(measureContainer);

    const widths: number[] = [];

    // Measure each cell in the column
    nodes.forEach((node) => {
      const renderedCell = getCellContent(node);
      if (!renderedCell) return;

      const cellContainer = document.createElement('div');
      const cellVnode = m(
        GridDataCell,
        {
          align: renderedCell.isNull
            ? 'center'
            : renderedCell.isNumerical
              ? 'right'
              : 'left',
          nullish: renderedCell.isNull,
          width: 'fit-content',
        },
        renderedCell.content,
      );

      m.render(cellContainer, cellVnode);
      measureContainer.appendChild(cellContainer);

      const cellElement = cellContainer.querySelector('.pf-grid__cell');
      if (cellElement) {
        widths.push(cellElement.scrollWidth);
      }

      measureContainer.removeChild(cellContainer);
    });

    // Measure header width
    const headerContainer = document.createElement('div');
    const headerVnode = m(
      GridHeaderCell,
      {width: 'fit-content'},
      headerText,
    );

    m.render(headerContainer, headerVnode);
    measureContainer.appendChild(headerContainer);

    const headerElement = headerContainer.querySelector('.pf-grid__cell');
    const headerWidth = headerElement ? headerElement.scrollWidth : 0;

    measureContainer.removeChild(headerContainer);
    document.body.removeChild(measureContainer);

    // Calculate 95th percentile of cell widths
    if (widths.length > 0) {
      widths.sort((a, b) => a - b);
      const percentileIndex = Math.ceil(widths.length * 0.95) - 1;
      const width95 = widths[Math.min(percentileIndex, widths.length - 1)];

      return Math.max(50, Math.ceil(Math.max(width95, headerWidth)));
    } else {
      return Math.max(50, Math.ceil(headerWidth));
    }
  }
}
