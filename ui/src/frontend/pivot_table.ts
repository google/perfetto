// Copyright (C) 2021 The Android Open Source Project
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

import * as m from 'mithril';

import {Actions} from '../common/actions';
import {
  ColumnAttrs,
  PivotTableQueryResponse,
  RowAttrs,
} from '../common/pivot_table_common';

import {globals} from './globals';
import {Panel} from './panel';
import {
  PivotTableHelper,
} from './pivot_table_helper';

interface ExpandableCellAttrs {
  pivotTableId: string;
  row: RowAttrs;
  column: ColumnAttrs;
  rowIndices: number[];
  expandedRowColumns: string[];
}

interface PivotTableRowAttrs {
  pivotTableId: string;
  row: RowAttrs;
  columns: ColumnAttrs[];
  rowIndices: number[];
  expandedRowColumns: string[];
}

interface PivotTableBodyAttrs {
  pivotTableId: string;
  rows: RowAttrs[];
  columns: ColumnAttrs[];
  rowIndices: number[];
  expandedRowColumns: string[];
}

interface PivotTableHeaderAttrs {
  helper: PivotTableHelper;
}

interface PivotTableAttrs {
  pivotTableId: string;
  helper?: PivotTableHelper;
}

class PivotTableHeader implements m.ClassComponent<PivotTableHeaderAttrs> {
  view(vnode: m.Vnode<PivotTableHeaderAttrs>) {
    const {helper} = vnode.attrs;
    const pivotTableId = helper.pivotTableId;
    const pivotTable = globals.state.pivotTable[pivotTableId];
    const resp =
        globals.queryResults.get(pivotTableId) as PivotTableQueryResponse;

    const cols = [];
    for (const column of resp.columns) {
      const isPivot = column.aggregation === undefined;
      let sortIcon;
      if (!isPivot) {
        sortIcon =
            column.order === 'DESC' ? 'arrow_drop_down' : 'arrow_drop_up';
      }
      cols.push(m(
          'td',
          {
            class: pivotTable.isLoadingQuery ? 'disabled' : '',
            draggable: !pivotTable.isLoadingQuery,
            ondragstart: (e: DragEvent) => {
              helper.selectedColumnOnDrag(e, isPivot, column.index);
            },
            ondrop: (e: DragEvent) => {
              helper.removeHighlightFromDropLocation(e);
              helper.selectedColumnOnDrop(e, isPivot, column.index);
              helper.queryPivotTableChanges();
            },
            ondragenter: (e: DragEvent) => {
              helper.highlightDropLocation(e, isPivot);
            },
            ondragleave: (e: DragEvent) => {
              helper.removeHighlightFromDropLocation(e);
            }
          },
          column.name,
          (!isPivot && sortIcon !== undefined ?
               m('i.material-icons',
                 {
                   onclick: () => {
                     if (!pivotTable.isLoadingQuery) {
                       helper.togglePivotTableAggregationSorting(column.index);
                       helper.queryPivotTableChanges();
                     }
                   }
                 },
                 sortIcon) :
               null),
          (!isPivot && resp.totalAggregations !== undefined ?
               m('.total-aggregation',
                 `(${resp.totalAggregations[column.name]})`) :
               null)));
    }
    return m('tr', cols);
  }
}

class ExpandableCell implements m.ClassComponent<ExpandableCellAttrs> {
  view(vnode: m.Vnode<ExpandableCellAttrs>) {
    const {pivotTableId, row, column, rowIndices, expandedRowColumns} =
        vnode.attrs;
    const pivotTable = globals.state.pivotTable[pivotTableId];
    let expandIcon = 'expand_more';
    if (row.expandedRows.has(column.name)) {
      expandIcon = row.expandedRows.get(column.name)!.isExpanded ?
          'expand_less' :
          'expand_more';
    }
    let spinnerVisibility = 'hidden';
    let animationState = 'paused';
    if (row.loadingColumn === column.name) {
      spinnerVisibility = 'visible';
      animationState = 'running';
    }
    const padValue = new Array(row.depth * 2).join(' ');

    return m(
        'td.allow-white-space',
        padValue,
        m('i.material-icons',
          {
            class: pivotTable.isLoadingQuery ? 'disabled' : '',
            onclick: () => {
              if (pivotTable.isLoadingQuery) {
                return;
              }
              const value = row.row[column.name]?.toString();
              if (value === undefined) {
                throw Error('Expanded row has undefined value.');
              }
              if (row.expandedRows.has(column.name) &&
                  row.expandedRows.get(column.name)!.isExpanded) {
                globals.dispatch(Actions.setPivotTableRequest({
                  pivotTableId,
                  action: 'UNEXPAND',
                  attrs: {
                    rowIndices,
                    columnIdx: column.index,
                    value,
                    expandedRowColumns
                  }
                }));
              } else {
                globals.dispatch(Actions.setPivotTableRequest({
                  pivotTableId,
                  action: column.isStackColumn ? 'DESCENDANTS' : 'EXPAND',
                  attrs: {
                    rowIndices,
                    columnIdx: column.index,
                    value,
                    expandedRowColumns
                  }
                }));
              }
            },
          },
          expandIcon),
        ' ',
        row.row[column.name],
        ' ',
        // Adds a loading spinner while querying the expanded column.
        m('.pivot-table-spinner', {
          style: {
            visibility: spinnerVisibility,
            animationPlayState: animationState
          }
        }));
  }
}

class PivotTableRow implements m.ClassComponent<PivotTableRowAttrs> {
  view(vnode: m.Vnode<PivotTableRowAttrs>) {
    const cells = [];
    const {pivotTableId, row, columns, rowIndices, expandedRowColumns} =
        vnode.attrs;

    for (const column of columns) {
      if (row.row[column.name] === undefined &&
          row.expandableColumns.has(column.name)) {
        throw Error(
            `Row data at expandable column "${column.name}" is undefined.`);
      }
      if (row.row[column.name] === undefined || row.row[column.name] === null) {
        cells.push(m('td', ''));
        continue;
      }
      if (row.expandableColumns.has(column.name)) {
        cells.push(
            m(ExpandableCell,
              {pivotTableId, row, column, rowIndices, expandedRowColumns}));
        continue;
      }

      let value = row.row[column.name]!.toString();
      if (column.aggregation === undefined) {
        // For each indentation level add 2 spaces, if we have an expansion
        // button add 3 spaces to cover the icon size.
        let padding = 2 * row.depth;
        if (row.depth > 0 && column.isStackColumn) {
          padding += 3;
        }
        value = value.padStart(padding + value.length, ' ');
      }
      cells.push(m('td.allow-white-space', value));
    }
    return m('tr', cells);
  }
}

class PivotTableBody implements m.ClassComponent<PivotTableBodyAttrs> {
  view(vnode: m.Vnode<PivotTableBodyAttrs>): m.Children {
    const pivotTableRows = [];
    const {pivotTableId, rows, columns, rowIndices, expandedRowColumns} =
        vnode.attrs;
    for (let i = 0; i < rows.length; ++i) {
      pivotTableRows.push(m(PivotTableRow, {
        pivotTableId,
        row: rows[i],
        columns,
        rowIndices: rowIndices.concat(i),
        expandedRowColumns
      }));
      for (const column of columns.slice().reverse()) {
        const expandedRows = rows[i].expandedRows.get(column.name);
        if (expandedRows !== undefined && expandedRows.isExpanded) {
          pivotTableRows.push(m(PivotTableBody, {
            pivotTableId,
            rows: expandedRows.rows,
            columns,
            rowIndices: rowIndices.concat(i),
            expandedRowColumns: expandedRowColumns.concat(column.name)
          }));
        }
      }
    }
    return pivotTableRows;
  }
}

export class PivotTable extends Panel<PivotTableAttrs> {
  view(vnode: m.CVnode<PivotTableAttrs>) {
    const {pivotTableId, helper} = vnode.attrs;
    const pivotTable = globals.state.pivotTable[pivotTableId];
    const resp =
        globals.queryResults.get(pivotTableId) as PivotTableQueryResponse;

    let body;
    let header;
    if (helper !== undefined && resp !== undefined) {
      header = m(PivotTableHeader, {helper});
      body = m(PivotTableBody, {
        pivotTableId,
        rows: resp.rows,
        columns: resp.columns,
        rowIndices: [],
        expandedRowColumns: []
      });
    }

    const startSec = pivotTable.traceTime ? pivotTable.traceTime.startSec :
                                            globals.state.traceTime.startSec;
    const endSec = pivotTable.traceTime ? pivotTable.traceTime.endSec :
                                          globals.state.traceTime.endSec;

    return m(
        'div.pivot-table-tab',
        m(
            'header.overview',
            m('span',
              m('button',
                {
                  disabled: helper === undefined || pivotTable.isLoadingQuery,
                  onclick: () => {
                    if (helper !== undefined) {
                      helper.toggleEditPivotTableModal();
                      globals.rafScheduler.scheduleFullRedraw();
                    }
                  }
                },
                'Edit'),
              ' ',
              (pivotTable.isLoadingQuery ? m('.pivot-table-spinner') : null),
              (resp !== undefined && !pivotTable.isLoadingQuery ?
                   m('span.code',
                     `Query took ${Math.round(resp.durationMs)} ms -`) :
                   null),
              m('span.code', `Selected range: ${endSec - startSec} s`)),
            m('button',
              {
                disabled: helper === undefined || pivotTable.isLoadingQuery,
                onclick: () => {
                  globals.frontendLocalState.togglePivotTable();
                  globals.queryResults.delete(pivotTableId);
                  globals.pivotTableHelper.delete(pivotTableId);
                  globals.dispatch(Actions.deletePivotTable({pivotTableId}));
                }
              },
              'Close'),
            ),
        m('.query-table-container',
          m('table.query-table.pivot-table',
            m('thead', header),
            m('tbody', body))));
  }

  renderCanvas() {}
}
