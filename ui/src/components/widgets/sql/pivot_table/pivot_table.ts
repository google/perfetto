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
import {
  BasicTable,
  ColumnDescriptor,
  ReorderableColumns,
} from '../../../../widgets/basic_table';
import {PivotTreeNode} from './pivot_tree_node';
import {Button} from '../../../../widgets/button';
import {Icons} from '../../../../base/semantic_icons';
import {sqlValueToReadableString} from '../../../../trace_processor/sql_utils';
import {LegacyTableColumn, tableColumnId} from '../legacy_table/table_column';
import {MenuDivider, MenuItem, PopupMenu} from '../../../../widgets/menu';
import {Anchor} from '../../../../widgets/anchor';
import {
  renderColumnIcon,
  renderSortMenuItems,
} from '../legacy_table/table_header';
import {SelectColumnMenu} from '../legacy_table/select_column_menu';
import {SqlColumn} from '../legacy_table/sql_column';
import {buildSqlQuery} from '../legacy_table/query_builder';
import {Aggregation, AGGREGATIONS} from './aggregations';
import {aggregationId, pivotId} from './ids';

export interface PivotTableAttrs {
  readonly state: PivotTableState;
}

export class PivotTable implements m.ClassComponent<PivotTableAttrs> {
  private readonly state: PivotTableState;

  constructor(vnode: m.CVnode<PivotTableAttrs>) {
    this.state = vnode.attrs.state;
  }

  view() {
    const data = this.state.getData();
    if (data === undefined) {
      return m(Spinner);
    }
    const pivotColumns: ColumnDescriptor<PivotTreeNode>[] = this.state
      .getPivots()
      .map((pivot, index) => ({
        title: this.renderPivotColumnHeader(pivot, index),
        render: (node) => [
          // Do not show the expand/collapse button for the last pivot.
          node.getPivotIndex() === index &&
            index + 1 !== this.state.getPivots().length &&
            m(Button, {
              icon: node.collapsed ? Icons.ExpandDown : Icons.ExpandUp,
              onclick: () => (node.collapsed = !node.collapsed),
            }),
          // Indent the expanded values to align them with the parent value
          // even though they do not have the "expand/collapse" button.
          index < node.getPivotIndex() && m('span.indent'),
          sqlValueToReadableString(node.getPivotValue(index)),
          // Show ellipsis for the last pivot if the node is collapsed to
          // make it clear to the user that there are some values.
          index > node.getPivotIndex() && node.collapsed && '...',
        ],
      }));

    const aggregationColumns: ColumnDescriptor<PivotTreeNode>[] = this.state
      .getAggregations()
      .map((agg, index) => ({
        title: this.renderAggregationColumnHeader(agg, index),
        render: (node) =>
          sqlValueToReadableString(node.getAggregationValue(index)),
      }));

    // Expand the tree to a list of rows to show.
    const nodes: PivotTreeNode[] = [...data.listDescendants()];

    return m(BasicTable<PivotTreeNode>, {
      className: 'pivot-table',
      data: nodes,
      columns: [
        new ReorderableColumns(pivotColumns, (from, to) =>
          this.state.movePivot(from, to),
        ),
        new ReorderableColumns(aggregationColumns, (from, to) =>
          this.state.moveAggregation(from, to),
        ),
      ],
    });
  }

  renderPivotColumnHeader(pivot: LegacyTableColumn, index: number) {
    const sorted = this.state.isSortedByPivot(pivot);
    return m(
      PopupMenu,
      {
        trigger: m(Anchor, {icon: renderColumnIcon(sorted)}, pivotId(pivot)),
      },
      [
        // Sort by pivot.
        renderSortMenuItems(sorted, (direction) =>
          this.state.sortByPivot(pivot, direction),
        ),
        // Remove pivot: show only if there is more than one pivot (to avoid
        // removing the last pivot).
        this.state.getPivots().length > 1 &&
          m(MenuItem, {
            label: 'Remove',
            icon: Icons.Delete,
            onclick: () => this.state.removePivot(index),
          }),

        // End of "per-pivot" menu items. The following menu items are table-level
        // operations (i.e. "add pivot").
        m(MenuDivider),

        m(
          MenuItem,
          {
            label: 'Add pivot',
            icon: Icons.AddColumn,
          },
          m(SelectColumnMenu, {
            columns: this.state.table.columns.map((column) => ({
              key: tableColumnId(column),
              column,
            })),
            manager: {
              filters: this.state.filters,
              trace: this.state.trace,
              getSqlQuery: (columns: {[key: string]: SqlColumn}) =>
                buildSqlQuery({
                  table: this.state.table.name,
                  columns,
                  filters: this.state.filters.get(),
                }),
            },
            existingColumnIds: new Set(this.state.getPivots().map(pivotId)),
            onColumnSelected: (column) => this.state.addPivot(column, index),
          }),
        ),
      ],
    );
  }

  renderAggregationColumnHeader(agg: Aggregation, index: number) {
    const sorted = this.state.isSortedByAggregation(agg);
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
          this.state.sortByAggregation(agg, direction),
        ),
        // Remove aggregation.
        // Do not remove count aggregation to ensure that there is always at least one aggregation.
        agg.op !== 'count' &&
          m(MenuItem, {
            label: 'Remove',
            icon: Icons.Delete,
            onclick: () => this.state.removeAggregation(index),
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
                  this.state.replaceAggregation(index, {
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
          onclick: () => this.state.addAggregation(agg, index + 1),
        }),

        // End of "per-pivot" menu items. The following menu items are table-level
        // operations (i.e. "add pivot").
        m(MenuDivider),

        m(
          MenuItem,
          {
            label: 'Add aggregation',
            icon: Icons.AddColumn,
          },
          m(SelectColumnMenu, {
            columns: this.state.table.columns.map((column) => ({
              key: tableColumnId(column),
              column,
            })),
            manager: {
              filters: this.state.filters,
              trace: this.state.trace,
              getSqlQuery: (columns: {[key: string]: SqlColumn}) =>
                buildSqlQuery({
                  table: this.state.table.name,
                  columns,
                  filters: this.state.filters.get(),
                }),
            },
            columnMenu: (column) => ({
              rightIcon: '',
              children: AGGREGATIONS.map((agg) =>
                m(MenuItem, {
                  label: agg,
                  onclick: () =>
                    this.state.addAggregation({op: agg, column}, index),
                }),
              ),
            }),
          }),
        ),
      ],
    );
  }
}
