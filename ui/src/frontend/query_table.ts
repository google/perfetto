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


import m from 'mithril';

import {Actions} from '../common/actions';
import {QueryResponse} from '../common/queries';
import {ColumnType, Row} from '../common/query_result';
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

// Convert column value to number if it's a bigint or a number, otherwise throw
function colToNumber(colValue: ColumnType): number {
  if (typeof colValue === 'bigint') {
    return Number(colValue);
  } else if (typeof colValue === 'number') {
    return colValue;
  } else {
    throw Error('Value is not a number or a bigint');
  }
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

    const sliceStart = fromNs(colToNumber(row.ts));
    // row.dur can be negative. Clamp to 1ns.
    const sliceDur = fromNs(Math.max(colToNumber(row.dur), 1));
    const sliceEnd = sliceStart + sliceDur;
    const trackId: number = colToNumber(row.track_id);
    const uiTrackId = globals.state.uiTrackIdByTraceTrackId[trackId];
    if (uiTrackId === undefined) return;
    verticalScrollToTrack(uiTrackId, true);
    // TODO(stevegolton) Soon this function will only accept Bigints
    focusHorizontalRange(sliceStart, sliceEnd);

    let sliceId: number|undefined;
    if (row.type?.toString().includes('slice')) {
      sliceId = colToNumber(row.id);
    } else {
      sliceId = colToNumber(row.slice_id);
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
      } else if (typeof value === 'bigint') {
        cells.push(m('td', value.toString()));
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

interface QueryTableContentAttrs {
  resp: QueryResponse;
}

class QueryTableContent implements m.ClassComponent<QueryTableContentAttrs> {
  private previousResponse?: QueryResponse;

  onbeforeupdate(vnode: m.CVnode<QueryTableContentAttrs>) {
    return vnode.attrs.resp !== this.previousResponse;
  }

  view(vnode: m.CVnode<QueryTableContentAttrs>) {
    const resp = vnode.attrs.resp;
    this.previousResponse = resp;
    const cols = [];
    for (const col of resp.columns) {
      cols.push(m('td', col));
    }
    const tableHeader = m('tr', cols);

    const rows =
        resp.rows.map((row) => m(QueryTableRow, {row, columns: resp.columns}));

    if (resp.error) {
      return m('.query-error', `SQL error: ${resp.error}`);
    } else {
      return m(
          '.query-table-container.x-scrollable',
          m('table.query-table', m('thead', tableHeader), m('tbody', rows)));
    }
  }
}

interface QueryTableAttrs {
  query: string;
  onClose: () => void;
  resp?: QueryResponse;
  contextButtons?: m.Child[];
}

export class QueryTable extends Panel<QueryTableAttrs> {
  view(vnode: m.CVnode<QueryTableAttrs>) {
    const resp = vnode.attrs.resp;

    const header: m.Child[] = [
      m('span',
        resp ? `Query result - ${Math.round(resp.durationMs)} ms` :
               `Query - running`),
      m('span.code.text-select', vnode.attrs.query),
      m('span.spacer'),
      ...(vnode.attrs.contextButtons ?? []),
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

    if (resp.statementWithOutputCount > 1) {
      headers.push(
          m('header.overview',
            `${resp.statementWithOutputCount} out of ${resp.statementCount} ` +
                `statements returned a result. Only the results for the last ` +
                `statement are displayed in the table below.`));
    }

    return m('div', ...headers, m(QueryTableContent, {resp}));
  }

  renderCanvas() {}
}
