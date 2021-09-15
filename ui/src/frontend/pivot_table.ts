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
} from '../common/pivot_table_data';

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
}

interface PivotTableRowAttrs {
  pivotTableId: string;
  row: RowAttrs;
  columns: ColumnAttrs[];
  rowIndices: number[];
}

interface PivotTableBodyAttrs {
  pivotTableId: string;
  rows: RowAttrs[];
  columns: ColumnAttrs[];
  rowIndices: number[];
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
            draggable: pivotTable.isLoadingQuery ? false : true,
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
               null)));
    }
    return m('tr', cols);
  }
}

class ExpandableCell implements m.ClassComponent<ExpandableCellAttrs> {
  view(vnode: m.Vnode<ExpandableCellAttrs>) {
    const {pivotTableId, row, column, rowIndices} = vnode.attrs;
    const pivotTable = globals.state.pivotTable[pivotTableId];
    const expandIcon = row.isExpanded ? 'expand_less' : 'expand_more';
    const spinnerVsibility = row.isLoadingQuery ? 'visible' : 'hidden';
    const animationState = row.isLoadingQuery ? 'running' : 'paused';

    return m(
        'td',
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
              if (row.isExpanded) {
                globals.dispatch(Actions.setPivotTableRequest({
                  pivotTableId,
                  action: 'UNEXPAND',
                  attrs: {
                    rowIndices,
                    columnIdx: column.index,
                    value,
                  }
                }));
              } else {
                globals.dispatch(Actions.setPivotTableRequest({
                  pivotTableId,
                  action: 'EXPAND',
                  attrs: {
                    rowIndices,
                    columnIdx: column.index,
                    value,
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
          style:
              {visibility: spinnerVsibility, animationPlayState: animationState}
        }));
  }
}

class PivotTableRow implements m.ClassComponent<PivotTableRowAttrs> {
  view(vnode: m.Vnode<PivotTableRowAttrs>) {
    const cells = [];
    const {pivotTableId, row, columns, rowIndices} = vnode.attrs;

    for (const column of columns) {
      if (row.row[column.name] === undefined &&
          row.expandableColumn === column.name) {
        throw Error(`Row data at expandable column "${
            row.expandableColumn}" is undefined.`);
      } else if (row.row[column.name] === undefined) {
        cells.push(m('td', ''));
      } else if (row.expandableColumn === column.name) {
        cells.push(m(ExpandableCell, {pivotTableId, row, column, rowIndices}));
      } else {
        let value = row.row[column.name]!.toString();
        if (column.aggregation !== undefined) {
          // Indenting the aggregations of expanded rows by 2 spaces.
          value =
              value.padStart(((rowIndices.length - 1) * 2) + value.length, ' ');
        }
        cells.push(m('td.allow-white-space', value));
      }
    }
    return m('tr', cells);
  }
}

class PivotTableBody implements m.ClassComponent<PivotTableBodyAttrs> {
  view(vnode: m.Vnode<PivotTableBodyAttrs>): m.Children {
    const pivotTableRows = [];
    const {pivotTableId, rows, columns, rowIndices} = vnode.attrs;
    for (let i = 0; i < rows.length; ++i) {
      pivotTableRows.push(m(PivotTableRow, {
        pivotTableId,
        row: rows[i],
        columns,
        rowIndices: rowIndices.concat(i)
      }));
      if (rows[i].isExpanded) {
        const expandedRows = rows[i].rows;
        if (expandedRows === undefined) {
          throw Error('Expanded row cannot have undefined rows');
        }
        pivotTableRows.push(m(PivotTableBody, {
          pivotTableId,
          rows: expandedRows,
          columns,
          rowIndices: rowIndices.concat(i)
        }));
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
        rowIndices: []
      });
    }

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
                     `Query took ${Math.round(resp.durationMs)} ms`) :
                   null)),
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
