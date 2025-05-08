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
import {MenuDivider, MenuItem, PopupMenu} from '../../../../widgets/menu';
import {Anchor} from '../../../../widgets/anchor';
import {renderColumnIcon, renderSortMenuItems} from '../table/table_header';
import {SelectColumnMenu} from '../table/select_column_menu';
import {SqlColumn} from '../table/sql_column';
import {buildSqlQuery} from '../table/query_builder';
import {Aggregation, AGGREGATIONS} from './aggregations';
import {aggregationId, pivotId} from './ids';
import {
  ColumnDescriptor,
  CustomTable,
  ReorderableColumns,
} from '../../../../widgets/custom_table';

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
    const pivotColumns: ColumnDescriptor<PivotTreeNode>[] = state
      .getPivots()
      .map((pivot, index) => ({
        title: this.renderPivotColumnHeader(attrs, pivot, index),
        render: (node) => {
          if (node.isRoot()) {
            return {
              cell: 'Total values:',
              className: 'total-values',
              colspan: state.getPivots().length,
            };
          }
          const status = node.getPivotDisplayStatus(index);
          const value = node.getPivotValue(index);
          return {
            cell: [
              (status === 'collapsed' || status === 'expanded') &&
                m(Button, {
                  icon:
                    status === 'collapsed' ? Icons.ExpandDown : Icons.ExpandUp,
                  onclick: () => (node.collapsed = !node.collapsed),
                }),
              // Show a non-clickable indicator that the value is auto-expanded.
              status === 'auto_expanded' &&
                m(Button, {
                  icon: 'chevron_right',
                  disabled: true,
                }),
              // Indent the expanded values to align them with the parent value
              // even though they do not have the "expand/collapse" button.
              status === 'pivoted_value' && m('span.indent'),
              value !== undefined && state.getPivots()[index].renderCell(value),
              // Show ellipsis for the last pivot if the node is collapsed to
              // make it clear to the user that there are some values.
              status === 'hidden_behind_collapsed' && '...',
            ],
          };
        },
      }));

    const aggregationColumns: ColumnDescriptor<PivotTreeNode>[] = state
      .getAggregations()
      .map((agg, index) => ({
        title: this.renderAggregationColumnHeader(attrs, agg, index),
        render: (node) => ({
          cell: agg.column.renderCell(node.getAggregationValue(index)),
        }),
      }));

    const extraRowButton = attrs.extraRowButton;
    const extraButtonColumn: ReorderableColumns<PivotTreeNode> | undefined =
      extraRowButton && {
        columns: [
          {
            title: undefined,
            render: (node) => ({
              cell: extraRowButton(node),
              className: 'action-button',
            }),
          },
        ],
        hasLeftBorder: false,
      };

    // Expand the tree to a list of rows to show.
    const nodes: PivotTreeNode[] = data ? [...data.listDescendants()] : [];

    return [
      m(CustomTable<PivotTreeNode>, {
        className: 'pivot-table',
        data: nodes,
        columns: [
          {
            columns: pivotColumns,
            reorder: (from, to) => state.movePivot(from, to),
          },
          {
            columns: aggregationColumns,
            reorder: (from, to) => state.moveAggregation(from, to),
          },
          extraButtonColumn,
        ],
      }),
      data === undefined && m(Spinner),
    ];
  }

  renderPivotColumnHeader(
    attrs: PivotTableAttrs,
    pivot: TableColumn,
    index: number,
  ) {
    const state = attrs.state;
    const sorted = state.isSortedByPivot(pivot);
    return m(
      PopupMenu,
      {
        trigger: m(Anchor, {icon: renderColumnIcon(sorted)}, pivotId(pivot)),
      },
      [
        // Sort by pivot.
        renderSortMenuItems(sorted, (direction) =>
          state.sortByPivot(pivot, direction),
        ),
        // Remove pivot: show only if there is more than one pivot (to avoid
        // removing the last pivot).
        state.getPivots().length > 1 &&
          m(MenuItem, {
            label: 'Remove',
            icon: Icons.Delete,
            onclick: () => state.removePivot(index),
          }),

        // End of "per-pivot" menu items. The following menu items are table-level
        // operations (i.e. "add pivot").
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
      ],
    );
  }

  renderAggregationColumnHeader(
    attrs: PivotTableAttrs,
    agg: Aggregation,
    index: number,
  ) {
    const state = attrs.state;
    const sorted = state.isSortedByAggregation(agg);
    return m(
      PopupMenu,
      {
        trigger: m(
          Anchor,
          {icon: renderColumnIcon(sorted)},
          aggregationId(agg),
        ),
      },
      [
        // Sort by aggregation.
        renderSortMenuItems(sorted, (direction) =>
          state.sortByAggregation(agg, direction),
        ),
        // Remove aggregation.
        // Do not remove count aggregation to ensure that there is always at least one aggregation.
        agg.op !== 'count' &&
          m(MenuItem, {
            label: 'Remove',
            icon: Icons.Delete,
            onclick: () => state.removeAggregation(index),
          }),
        // Change aggregation operation.
        // Do not change aggregation for count (as it's the only one which doesn't require a column).
        agg.op !== 'count' &&
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
      ],
    );
  }
}
