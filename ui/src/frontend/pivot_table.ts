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
import {QueryResponse} from '../common/queries';
import {Row} from '../common/query_result';

import {queryResponseToClipboard} from './clipboard';
import {globals} from './globals';
import {Panel} from './panel';

interface PivotTableRowAttrs {
  row: Row;
  columns: string[];
}

class PivotTableRow implements m.ClassComponent<PivotTableRowAttrs> {
  view(vnode: m.Vnode<PivotTableRowAttrs>) {
    const cells = [];
    const {row, columns} = vnode.attrs;
    for (const col of columns) {
      cells.push(m('td', row[col]));
    }

    return m('tr', cells);
  }
}

interface PivotTableAttrs {
  pivotTableId: string;
}

class ColumnPicker implements m.ClassComponent<PivotTableAttrs> {
  view(vnode: m.Vnode<PivotTableAttrs>) {
    const {pivotTableId} = vnode.attrs;
    const availableColumns = globals.state.pivotTableConfig.availableColumns;
    const availableColumnsCount =
        globals.state.pivotTableConfig.totalColumnsCount;
    const availableAggregations =
        globals.state.pivotTableConfig.availableAggregations;
    if (availableColumns === undefined || availableColumnsCount === undefined) {
      return 'Loading columns...';
    }
    if (availableAggregations === undefined) {
      return 'Loading aggregations...';
    }
    if (availableColumnsCount === 0) {
      return 'No columns available';
    }
    if (availableAggregations.length === 0) {
      return 'No aggregations available';
    }

    if (globals.state.pivotTable[pivotTableId].selectedColumnIndex ===
        undefined) {
      globals.state.pivotTable[pivotTableId].selectedColumnIndex = 0;
    }
    if (globals.state.pivotTable[pivotTableId].selectedAggregationIndex ===
        undefined) {
      globals.state.pivotTable[pivotTableId].selectedAggregationIndex = 0;
    }

    // Fills available aggregations options in aggregation select.
    const aggregationOptions = [];
    for (let i = 0; i < availableAggregations.length; ++i) {
      aggregationOptions.push(
          m('option',
            {value: availableAggregations[i], key: availableAggregations[i]},
            availableAggregations[i]));
    }

    // Fills available columns options divided according to their table in
    // column select.
    const columnOptionGroup = [];
    for (let i = 0; i < availableColumns.length; ++i) {
      const options = [];
      for (let j = 0; j < availableColumns[i].columns.length; ++j) {
        options.push(
            m('option',
              {
                value: availableColumns[i].columns[j],
                key: availableColumns[i].columns[j]
              },
              availableColumns[i].columns[j]));
      }
      columnOptionGroup.push(
          m('optgroup', {label: availableColumns[i].tableName}, options));
    }

    return m('div', [
      'Select a column: ',
      // Pivot radio button.
      m(`input[type=radio][name=type][id=pivot]`, {
        checked: globals.state.pivotTable[pivotTableId].isPivot,
        onchange: () =>
            globals.dispatch(Actions.togglePivotSelection({pivotTableId}))
      }),
      m(`label[for=pivot]`, 'Pivot'),
      // Aggregation radio button.
      m(`input[type=radio][name=type][id=aggregation]`, {
        checked: !globals.state.pivotTable[pivotTableId].isPivot,
        onchange: () =>
            globals.dispatch(Actions.togglePivotSelection({pivotTableId}))
      }),
      m(`label[for=aggregation]`, 'Aggregation'),
      ' ',
      // Aggregation select.
      m('select',
        {
          disabled: (globals.state.pivotTable[pivotTableId].isPivot === true),
          selectedIndex:
              globals.state.pivotTable[pivotTableId].selectedAggregationIndex,
          onchange: (e: InputEvent) => {
            globals.dispatch(Actions.setSelectedPivotTableAggregationIndex({
              pivotTableId,
              index: (e.target as HTMLSelectElement).selectedIndex
            }));
          }
        },
        aggregationOptions),
      ' ',
      // Column select.
      m('select',
        {
          selectedIndex:
              globals.state.pivotTable[pivotTableId].selectedColumnIndex,
          onchange: (e: InputEvent) => {
            globals.dispatch(Actions.setSelectedPivotTableColumnIndex({
              pivotTableId,
              index: (e.target as HTMLSelectElement).selectedIndex
            }));
          }
        },
        columnOptionGroup),
      ' ',
      // Button to toggle selected column.
      m('button.query-ctrl',
        {
          onclick: () => {
            globals.dispatch(
                Actions.setPivotTableRequest({pivotTableId, action: 'UPDATE'}));
          }
        },
        'Add/Remove'),
      // Button to execute query based on added/removed columns.
      m('button.query-ctrl',
        {
          onclick: () => {
            globals.dispatch(
                Actions.setPivotTableRequest({pivotTableId, action: 'QUERY'}));
          }
        },
        'Query'),
      // Button to clear table and all selected columns.
      m('button.query-ctrl',
        {
          onclick: () => {
            globals.dispatch(Actions.clearPivotTableColumns({pivotTableId}));
            globals.dispatch(
                Actions.setPivotTableRequest({pivotTableId, action: 'QUERY'}));
          }
        },
        'Clear'),
    ]);
  }
}

export class PivotTable extends Panel<PivotTableAttrs> {
  view(vnode: m.CVnode<PivotTableAttrs>) {
    const {pivotTableId} = vnode.attrs;
    const resp = globals.queryResults.get(pivotTableId) as QueryResponse;
    // Query resulting from query generator should always be valid.
    if (resp !== undefined && resp.error) {
      throw Error(`Pivot table query resulted in SQL error: ${resp.error}`);
    }
    const cols = [];
    const rows = [];
    let header;

    if (resp !== undefined) {
      for (const col of resp.columns) {
        cols.push(m('td', col));
      }
      header = m('tr', cols);

      for (let i = 0; i < resp.rows.length; i++) {
        rows.push(m(PivotTableRow, {row: resp.rows[i], columns: resp.columns}));
      }
    }

    return m(
        'div',
        m(
            'header.overview',
            m(
                'span.code',
                m(ColumnPicker, {pivotTableId}),
                ),
            (resp === undefined || resp.error) ?
                null :
                m('button.query-ctrl',
                  {
                    onclick: () => {
                      queryResponseToClipboard(resp);
                    },
                  },
                  'Copy as .tsv'),
            m('button.query-ctrl',
              {
                onclick: () => {
                  globals.frontendLocalState.togglePivotTable();
                  globals.dispatch(Actions.deletePivotTable({pivotTableId}));
                }
              },
              'Close'),
            ),
        m('query-table-container',
          m('table.query-table', m('thead', header), m('tbody', rows))));
  }

  renderCanvas() {}
}
