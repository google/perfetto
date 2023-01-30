// Copyright (C) 2020 The Android Open Source Project
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
import {fromNs} from '../common/time';

import {copyToClipboard, queryResponseToClipboard} from './clipboard';
import {globals} from './globals';
import {Panel} from './panel';
import {Router} from './router';
import {
  focusHorizontalRange,
  verticalScrollToTrack,
} from './scroll_helper';

interface QueryTableRowAttrs {
  row: Row;
  columns: string[];
}

class QueryTableRow implements m.ClassComponent<QueryTableRowAttrs> {
  static columnsContainsSliceLocation(columns: string[]) {
    const requiredColumns = ['ts', 'dur', 'track_id'];
    for (const col of requiredColumns) {
      if (!columns.includes(col)) return false;
    }
    return true;
  }

  static rowOnClickHandler(
      event: Event, row: Row, nextTab: 'CurrentSelection'|'QueryResults') {
    // TODO(dproy): Make click handler work from analyze page.
    if (Router.parseUrl(window.location.href).page !== '/viewer') return;
    // If the click bubbles up to the pan and zoom handler that will deselect
    // the slice.
    event.stopPropagation();

    const sliceStart = fromNs(row.ts as number);
    // row.dur can be negative. Clamp to 1ns.
    const sliceDur = fromNs(Math.max(row.dur as number, 1));
    const sliceEnd = sliceStart + sliceDur;
    const trackId = row.track_id as number;
    const uiTrackId = globals.state.uiTrackIdByTraceTrackId[trackId];
    if (uiTrackId === undefined) return;
    verticalScrollToTrack(uiTrackId, true);
    focusHorizontalRange(sliceStart, sliceEnd);
    let sliceId: number|undefined;
    if (row.type?.toString().includes('slice')) {
      sliceId = row.id as number | undefined;
    } else {
      sliceId = row.slice_id as number | undefined;
    }
    if (sliceId !== undefined) {
      globals.makeSelection(
          Actions.selectChromeSlice(
              {id: sliceId, trackId: uiTrackId, table: 'slice'}),
          nextTab === 'QueryResults' ? globals.state.currentTab :
                                       'current_selection');
    }
  }

  view(vnode: m.Vnode<QueryTableRowAttrs>) {
    const cells = [];
    const {row, columns} = vnode.attrs;
    for (const col of columns) {
      const value = row[col];
      if (value instanceof Uint8Array) {
        cells.push(m('td', `<BLOB sz=${value.length}>`));
      } else {
        cells.push(m('td', value));
      }
    }
    const containsSliceLocation =
        QueryTableRow.columnsContainsSliceLocation(columns);
    const maybeOnClick = containsSliceLocation ?
        (e: Event) => QueryTableRow.rowOnClickHandler(e, row, 'QueryResults') :
        null;
    const maybeOnDblClick = containsSliceLocation ?
        (e: Event) =>
            QueryTableRow.rowOnClickHandler(e, row, 'CurrentSelection') :
        null;
    return m(
        'tr',
        {
          'onclick': maybeOnClick,
          // TODO(altimin): Consider improving the logic here (e.g. delay?) to
          // account for cases when dblclick fires late.
          'ondblclick': maybeOnDblClick,
          'clickable': containsSliceLocation,
        },
        cells);
  }
}

interface QueryTableAttrs {
  queryId: string;
}

export class QueryTable extends Panel<QueryTableAttrs> {
  private previousResponse?: QueryResponse;

  onbeforeupdate(vnode: m.CVnode<QueryTableAttrs>) {
    const {queryId} = vnode.attrs;
    const resp = globals.queryResults.get(queryId) as QueryResponse;
    const res = resp !== this.previousResponse;
    return res;
  }

  view(vnode: m.CVnode<QueryTableAttrs>) {
    const {queryId} = vnode.attrs;
    const resp = globals.queryResults.get(queryId) as QueryResponse;
    if (resp === undefined) {
      return m('');
    }
    this.previousResponse = resp;
    const cols = [];
    for (const col of resp.columns) {
      cols.push(m('td', col));
    }
    const header = m('tr', cols);

    const rows = [];
    for (let i = 0; i < resp.rows.length; i++) {
      rows.push(m(QueryTableRow, {row: resp.rows[i], columns: resp.columns}));
    }

    const headers = [
      m(
          'header.overview',
          m('span', `Query result - ${Math.round(resp.durationMs)} ms`),
          m('span.code.text-select', resp.query),
          m('span.spacer'),
          m('button.query-ctrl',
            {
              onclick: () => {
                copyToClipboard(resp.query);
              },
            },
            'Copy query'),
          resp.error ? null :
                       m('button.query-ctrl',
                         {
                           onclick: () => {
                             queryResponseToClipboard(resp);
                           },
                         },
                         'Copy result (.tsv)'),
          m('button.query-ctrl',
            {
              onclick: () => {
                globals.queryResults.delete(queryId);
                globals.rafScheduler.scheduleFullRedraw();
              },
            },
            'Close'),
          ),
    ];


    if (resp.statementWithOutputCount > 1) {
      headers.push(
          m('header.overview',
            `${resp.statementWithOutputCount} out of ${resp.statementCount} ` +
                `statements returned a result. Only the results for the last ` +
                `statement are displayed in the table below.`));
    }

    return m(
        'div',
        ...headers,
        resp.error ?
            m('.query-error', `SQL error: ${resp.error}`) :
            m('.query-table-container.x-scrollable',
              m('table.query-table', m('thead', header), m('tbody', rows))));
  }

  renderCanvas() {}
}
