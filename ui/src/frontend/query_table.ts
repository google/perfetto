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
import {Anchor} from './anchor';

import {copyToClipboard, queryResponseToClipboard} from './clipboard';
import {downloadData} from './download_utils';
import {globals} from './globals';
import {Panel} from './panel';
import {Router} from './router';
import {
  focusHorizontalRange,
  verticalScrollToTrack,
} from './scroll_helper';
import {Button} from './widgets/button';

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
        cells.push(
            m('td',
              m(Anchor,
                {
                  onclick: () => downloadData(`${col}.blob`, value),
                },
                `Blob (${value.length} bytes)`)));
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
  query: string;
  resp?: QueryResponse;
  onClose: () => void;
}

export class QueryTable extends Panel<QueryTableAttrs> {
  private previousResponse?: QueryResponse;

  onbeforeupdate(vnode: m.CVnode<QueryTableAttrs>) {
    return vnode.attrs.resp !== this.previousResponse;
  }

  view(vnode: m.CVnode<QueryTableAttrs>) {
    const resp = vnode.attrs.resp;

    const header: m.Child[] = [
      m('span',
        resp ? `Query result - ${Math.round(resp.durationMs)} ms` :
               `Query - running`),
      m('span.code.text-select', vnode.attrs.query),
      m('span.spacer'),
      m(Button, {
        label: 'Copy query',
        minimal: true,
        onclick: () => {
          copyToClipboard(vnode.attrs.query);
        },
      }),
    ];
    if (resp) {
      if (resp.error === undefined) {
        header.push(m(Button, {
          label: 'Copy result (.tsv)',
          minimal: true,
          onclick: () => {
            queryResponseToClipboard(resp);
          },
        }));
      }
    }
    header.push(m(Button, {
      label: 'Close',
      minimal: true,
      onclick: () => vnode.attrs.onClose(),
    }));

    const headers = [m('header.overview', ...header)];

    if (resp === undefined) {
      return m('div', ...headers);
    }

    this.previousResponse = resp;
    const cols = [];
    for (const col of resp.columns) {
      cols.push(m('td', col));
    }
    const tableHeader = m('tr', cols);

    const rows = [];
    for (let i = 0; i < resp.rows.length; i++) {
      rows.push(m(QueryTableRow, {row: resp.rows[i], columns: resp.columns}));
    }

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
        resp.error ? m('.query-error', `SQL error: ${resp.error}`) :
                     m('.query-table-container.x-scrollable',
                       m('table.query-table',
                         m('thead', tableHeader),
                         m('tbody', rows))));
  }

  renderCanvas() {}
}
