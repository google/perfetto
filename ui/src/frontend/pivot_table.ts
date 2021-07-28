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

const PLACEHOLDER = 'Enter SQL query';
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

class PivotTableQuery {
  private query = '';

  setQuery(value: string) {
    this.query = value;
  }

  getQuery(): string {
    return this.query;
  }
}

interface PivotTableAttrs {
  queryId: string;
}

export class PivotTable extends Panel<PivotTableAttrs> {
  private previousResponse?: QueryResponse;
  private pivotTableQuery: PivotTableQuery = new PivotTableQuery();

  onbeforeupdate(vnode: m.CVnode<PivotTableAttrs>) {
    const {queryId} = vnode.attrs;
    const resp = globals.queryResults.get(queryId) as QueryResponse;
    const res = resp !== this.previousResponse;
    return res;
  }

  view(vnode: m.CVnode<PivotTableAttrs>) {
    const {queryId} = vnode.attrs;
    const resp = globals.queryResults.get(queryId) as QueryResponse;
    const cols = [];
    const rows = [];
    let header;

    if (resp !== undefined) {
      this.previousResponse = resp;
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
                m('input', {
                  placeholder: PLACEHOLDER,
                  oninput: (e: InputEvent) => {
                    this.pivotTableQuery.setQuery(
                        (e.target as HTMLInputElement).value);
                  }
                }),
                m('button.query-ctrl',
                  {
                    onclick: () => {
                      if (this.pivotTableQuery.getQuery().length === 0) return;
                      globals.dispatch(Actions.executeQuery({
                        engineId: '0',
                        queryId: 'pivot-table-query',
                        query: this.pivotTableQuery.getQuery()
                      }));
                    }
                  },
                  'Query'),
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
                }
              },
              'Close'),
            ),
        (resp !== undefined && resp.error) ?
            m('.query-error', `SQL error: ${resp.error}`) :
            m('query-table-container',
              m('table.query-table', m('thead', header), m('tbody', rows))));
  }

  renderCanvas() {}
}
