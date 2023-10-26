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

import {BigintMath} from '../base/bigint_math';
import {copyToClipboard} from '../base/clipboard';
import {isString} from '../base/object_utils';
import {Duration, Time} from '../base/time';
import {Actions} from '../common/actions';
import {QueryResponse} from '../common/queries';
import {Row} from '../common/query_result';
import {Anchor} from '../widgets/anchor';
import {Button} from '../widgets/button';
import {Callout} from '../widgets/callout';
import {DetailsShell} from '../widgets/details_shell';

import {queryResponseToClipboard} from './clipboard';
import {downloadData} from './download_utils';
import {globals} from './globals';
import {Router} from './router';
import {reveal} from './scroll_helper';

interface QueryTableRowAttrs {
  row: Row;
  columns: string[];
}

type Numeric = bigint|number;

function isIntegral(x: Row[string]): x is Numeric {
  return typeof x === 'bigint' ||
      (typeof x === 'number' && Number.isInteger(x));
}

function hasTs(row: Row): row is Row&{ts: Numeric} {
  return ('ts' in row && isIntegral(row.ts));
}

function hasDur(row: Row): row is Row&{dur: Numeric} {
  return ('dur' in row && isIntegral(row.dur));
}

function hasTrackId(row: Row): row is Row&{track_id: Numeric} {
  return ('track_id' in row && isIntegral(row.track_id));
}

function hasType(row: Row): row is Row&{type: string} {
  return ('type' in row && isString(row.type));
}

function hasId(row: Row): row is Row&{id: Numeric} {
  return ('id' in row && isIntegral(row.id));
}

function hasSliceId(row: Row): row is Row&{slice_id: Numeric} {
  return ('slice_id' in row && isIntegral(row.slice_id));
}

// These are properties that a row should have in order to be "slice-like",
// insofar as it represents a time range and a track id which can be revealed
// or zoomed-into on the timeline.
type Sliceish = {
  ts: Numeric,
  dur: Numeric,
  track_id: Numeric
};

export function isSliceish(row: Row): row is Row&Sliceish {
  return hasTs(row) && hasDur(row) && hasTrackId(row);
}

// Attempts to extract a slice ID from a row, or undefined if none can be found
export function getSliceId(row: Row): number|undefined {
  if (hasType(row) && row.type.includes('slice')) {
    if (hasId(row)) {
      return Number(row.id);
    }
  } else {
    if (hasSliceId(row)) {
      return Number(row.slice_id);
    }
  }
  return undefined;
}

class QueryTableRow implements m.ClassComponent<QueryTableRowAttrs> {
  view(vnode: m.Vnode<QueryTableRowAttrs>) {
    const {row, columns} = vnode.attrs;
    const cells = columns.map((col) => this.renderCell(col, row[col]));

    // TODO(dproy): Make click handler work from analyze page.
    if (Router.parseUrl(window.location.href).page === '/viewer' &&
        isSliceish(row)) {
      return m(
          'tr',
          {
            onclick: () => this.highlightSlice(row, globals.state.currentTab),
            // TODO(altimin): Consider improving the logic here (e.g. delay?) to
            // account for cases when dblclick fires late.
            ondblclick: () => this.highlightSlice(row),
            clickable: true,
            title: 'Go to slice',
          },
          cells);
    } else {
      return m('tr', cells);
    }
  }

  private renderCell(name: string, value: Row[string]) {
    if (value instanceof Uint8Array) {
      return m('td', this.renderBlob(name, value));
    } else {
      return m('td', `${value}`);
    }
  }

  private renderBlob(name: string, value: Uint8Array) {
    return m(
        Anchor,
        {
          onclick: () => downloadData(`${name}.blob`, value),
        },
        `Blob (${value.length} bytes)`);
  }

  private highlightSlice(row: Row&Sliceish, nextTab?: string) {
    const trackId = Number(row.track_id);
    const sliceStart = Time.fromRaw(BigInt(row.ts));
    // row.dur can be negative. Clamp to 1ns.
    const sliceDur = BigintMath.max(BigInt(row.dur), 1n);
    const trackKey = globals.state.trackKeyByTrackId[trackId];
    if (trackKey !== undefined) {
      reveal(trackKey, sliceStart, Time.add(sliceStart, sliceDur), true);
      const sliceId = getSliceId(row);
      if (sliceId !== undefined) {
        this.selectSlice(sliceId, trackKey, nextTab);
      }
    }
  }

  private selectSlice(sliceId: number, trackKey: string, nextTab?: string) {
    const action = Actions.selectChromeSlice({
      id: sliceId,
      trackKey,
      table: 'slice',
    });
    globals.makeSelection(action, {tab: nextTab});
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
          'table.pf-query-table', m('thead', tableHeader), m('tbody', rows));
    }
  }
}

interface QueryTableAttrs {
  query: string;
  onClose: () => void;
  resp?: QueryResponse;
  contextButtons?: m.Child[];
  fillParent: boolean;
}

export class QueryTable implements m.ClassComponent<QueryTableAttrs> {
  view({attrs}: m.CVnode<QueryTableAttrs>) {
    const {
      resp,
      query,
      onClose,
      contextButtons = [],
      fillParent,
    } = attrs;

    return m(
        DetailsShell,
        {
          title: this.renderTitle(resp),
          description: query,
          buttons: this.renderButtons(query, onClose, contextButtons, resp),
          fillParent,
        },
        resp && this.renderTableContent(resp),
    );
  }

  renderTitle(resp?: QueryResponse) {
    if (!resp) {
      return 'Query - running';
    }
    const result = resp.error ? 'error' : `${resp.rows.length} rows`;
    const dur = Duration.humanise(Duration.fromMillis(resp.durationMs));
    return `Query result (${result}) - ${dur}`;
  }

  renderButtons(
      query: string, onClose: () => void, contextButtons: m.Child[],
      resp?: QueryResponse) {
    return [
      contextButtons,
      m(Button, {
        label: 'Copy query',
        minimal: true,
        onclick: () => {
          copyToClipboard(query);
        },
      }),
      (resp && resp.error === undefined) && m(Button, {
        label: 'Copy result (.tsv)',
        minimal: true,
        onclick: () => {
          queryResponseToClipboard(resp);
        },
      }),
      m(Button, {
        minimal: true,
        label: 'Close',
        onclick: onClose,
      }),
    ];
  }

  renderTableContent(resp: QueryResponse) {
    return m(
        '.pf-query-panel',
        resp.statementWithOutputCount > 1 &&
            m('.pf-query-warning',
              m(
                  Callout,
                  {icon: 'warning'},
                  `${resp.statementWithOutputCount} out of ${
                      resp.statementCount} `,
                  'statements returned a result. ',
                  'Only the results for the last statement are displayed.',
                  )),
        m(QueryTableContent, {resp}),
    );
  }
}
