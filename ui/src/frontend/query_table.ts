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
import {Row} from '../common/protos';
import {QueryResponse} from '../common/queries';
import {fromNs} from '../common/time';

import {copyToClipboard} from './clipboard';
import {globals} from './globals';
import {Panel} from './panel';
import {
  findUiTrackId,
  horizontalScrollAndZoomToRange,
  verticalScrollToTrack
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
    if (globals.state.route !== '/viewer') return;
    // If the click bubbles up to the pan and zoom handler that will deselect
    // the slice.
    event.stopPropagation();

    const sliceStart = fromNs(row.ts as number);
    // row.dur can be negative. Clamp to 1ns.
    const sliceDur = fromNs(Math.max(row.dur as number, 1));
    const sliceEnd = sliceStart + sliceDur;
    const trackId = row.track_id as number;
    const uiTrackId = findUiTrackId(trackId);
    if (uiTrackId === null) return;
    verticalScrollToTrack(uiTrackId, true);
    horizontalScrollAndZoomToRange(sliceStart, sliceEnd);
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
          nextTab === 'QueryResults' ? globals.frontendLocalState.currentTab :
                                       'current_selection');
    }
  }

  view(vnode: m.Vnode<QueryTableRowAttrs>) {
    const cells = [];
    const {row, columns} = vnode.attrs;
    for (const col of columns) {
      cells.push(m('td', row[col]));
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
          onclick: maybeOnClick,
          // TODO(altimin): Consider improving the logic here (e.g. delay?) to
          // account for cases when dblclick fires late.
          ondblclick: maybeOnDblClick,
          'clickable': containsSliceLocation
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

    return m(
        'div',
        m(
            'header.overview',
            `Query result - ${Math.round(resp.durationMs)} ms`,
            m('span.code', resp.query),
            resp.error ?
                null :
                m('button.query-ctrl',
                  {
                    onclick: () => {
                      const lines: string[][] = [];
                      lines.push(resp.columns);
                      for (const row of resp.rows) {
                        const line = [];
                        for (const col of resp.columns) {
                          line.push(row[col].toString());
                        }
                        lines.push(line);
                      }
                      copyToClipboard(
                          lines.map(line => line.join('\t')).join('\n'));
                    },
                  },
                  'Copy as .tsv'),
            m('button.query-ctrl',
              {
                onclick: () => {
                  globals.queryResults.delete(queryId);
                  globals.rafScheduler.scheduleFullRedraw();
                }
              },
              'Close'),
            ),
        resp.error ?
            m('.query-error', `SQL error: ${resp.error}`) :
            m('.query-table-container',
              m('table.query-table', m('thead', header), m('tbody', rows))));
  }

  renderCanvas() {}
}
