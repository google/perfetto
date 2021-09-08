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
} from '../common/pivot_table_data';
import {Row} from '../common/query_result';

import {globals} from './globals';
import {Panel} from './panel';
import {
  PivotTableHelper,
} from './pivot_table_helper';

interface PivotTableRowAttrs {
  row: Row;
  columns: ColumnAttrs[];
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

class PivotTableRow implements m.ClassComponent<PivotTableRowAttrs> {
  view(vnode: m.Vnode<PivotTableRowAttrs>) {
    const cells = [];
    const {row, columns} = vnode.attrs;
    for (const col of columns) {
      cells.push(m('td', row[col.name]));
    }

    return m('tr', cells);
  }
}

export class PivotTable extends Panel<PivotTableAttrs> {
  view(vnode: m.CVnode<PivotTableAttrs>) {
    const {pivotTableId, helper} = vnode.attrs;
    const pivotTable = globals.state.pivotTable[pivotTableId];
    const resp =
        globals.queryResults.get(pivotTableId) as PivotTableQueryResponse;

    // Query resulting from query generator should always be valid.
    if (resp !== undefined && resp.error) {
      throw Error(`Pivot table query resulted in SQL error: ${resp.error}`);
    }

    const rows = [];
    let header;

    if (helper !== undefined && resp !== undefined) {
      header = m(PivotTableHeader, {helper});

      for (const row of resp.rows) {
        rows.push(m(PivotTableRow, {row, columns: resp.columns}));
      }
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
              (pivotTable.isLoadingQuery ? m('div.pivot-table-spinner') : null),
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
          m('table.query-table',
            m('thead.pivot-table-header', header),
            m('tbody', rows))));
  }

  renderCanvas() {}
}
