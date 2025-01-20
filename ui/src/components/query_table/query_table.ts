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
import {copyToClipboard} from '../../base/clipboard';
import {QueryResponse} from './queries';
import {Row} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';
import {Button} from '../../widgets/button';
import {Callout} from '../../widgets/callout';
import {DetailsShell} from '../../widgets/details_shell';
import {downloadData} from '../../base/download_utils';
import {Router} from '../../core/router';
import {AppImpl} from '../../core/app_impl';
import {Trace} from '../../public/trace';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {Icons} from '../../base/semantic_icons';

interface QueryTableRowAttrs {
  trace: Trace;
  row: Row;
  columns: string[];
}

type Numeric = bigint | number;

function isIntegral(x: Row[string]): x is Numeric {
  return (
    typeof x === 'bigint' || (typeof x === 'number' && Number.isInteger(x))
  );
}

function hasTs(row: Row): row is Row & {ts: Numeric} {
  return 'ts' in row && isIntegral(row.ts);
}

function hasDur(row: Row): row is Row & {dur: Numeric} {
  return 'dur' in row && isIntegral(row.dur);
}

function hasTrackId(row: Row): row is Row & {track_id: Numeric} {
  return 'track_id' in row && isIntegral(row.track_id);
}

function hasSliceId(row: Row): row is Row & {slice_id: Numeric} {
  return 'slice_id' in row && isIntegral(row.slice_id);
}

// These are properties that a row should have in order to be "slice-like",
// insofar as it represents a time range and a track id which can be revealed
// or zoomed-into on the timeline.
type Sliceish = {
  ts: Numeric;
  dur: Numeric;
  track_id: Numeric;
};

export function isSliceish(row: Row): row is Row & Sliceish {
  return hasTs(row) && hasDur(row) && hasTrackId(row);
}

// Attempts to extract a slice ID from a row, or undefined if none can be found
export function getSliceId(row: Row): number | undefined {
  if (hasSliceId(row)) {
    return Number(row.slice_id);
  }
  return undefined;
}

class QueryTableRow implements m.ClassComponent<QueryTableRowAttrs> {
  private readonly trace: Trace;

  constructor({attrs}: m.Vnode<QueryTableRowAttrs>) {
    this.trace = attrs.trace;
  }

  view(vnode: m.Vnode<QueryTableRowAttrs>) {
    const {row, columns} = vnode.attrs;
    const cells = columns.map((col) => this.renderCell(col, row[col]));

    // TODO(dproy): Make click handler work from analyze page.
    if (
      Router.parseUrl(window.location.href).page === '/viewer' &&
      isSliceish(row)
    ) {
      return m(
        'tr',
        {
          onclick: () => this.selectAndRevealSlice(row, false),
          // TODO(altimin): Consider improving the logic here (e.g. delay?) to
          // account for cases when dblclick fires late.
          ondblclick: () => this.selectAndRevealSlice(row, true),
          clickable: true,
          title: 'Go to slice',
        },
        cells,
      );
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
      `Blob (${value.length} bytes)`,
    );
  }

  private selectAndRevealSlice(
    row: Row & Sliceish,
    switchToCurrentSelectionTab: boolean,
  ) {
    const sliceId = getSliceId(row);
    if (sliceId === undefined) {
      return;
    }
    this.trace.selection.selectSqlEvent('slice', sliceId, {
      switchToCurrentSelectionTab,
      scrollToSelection: true,
    });
  }
}

interface QueryTableContentAttrs {
  trace: Trace;
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

    const rows = resp.rows.map((row) =>
      m(QueryTableRow, {trace: vnode.attrs.trace, row, columns: resp.columns}),
    );

    if (resp.error) {
      return m('.query-error', `SQL error: ${resp.error}`);
    } else {
      return m(
        'table.pf-query-table',
        m('thead', tableHeader),
        m('tbody', rows),
      );
    }
  }
}

interface QueryTableAttrs {
  trace: Trace;
  query: string;
  resp?: QueryResponse;
  contextButtons?: m.Child[];
  fillParent: boolean;
}

export class QueryTable implements m.ClassComponent<QueryTableAttrs> {
  private readonly trace: Trace;

  constructor({attrs}: m.CVnode<QueryTableAttrs>) {
    this.trace = attrs.trace;
  }

  view({attrs}: m.CVnode<QueryTableAttrs>) {
    const {resp, query, contextButtons = [], fillParent} = attrs;

    return m(
      DetailsShell,
      {
        title: this.renderTitle(resp),
        description: query,
        buttons: this.renderButtons(query, contextButtons, resp),
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
    if (AppImpl.instance.testingMode) {
      // Omit the duration in tests, they cause screenshot diff failures.
      return `Query result (${result})`;
    }
    return `Query result (${result}) - ${resp.durationMs.toLocaleString()}ms`;
  }

  renderButtons(
    query: string,
    contextButtons: m.Child[],
    resp?: QueryResponse,
  ) {
    return [
      contextButtons,
      m(
        PopupMenu,
        {
          trigger: m(Button, {
            label: 'Copy',
            rightIcon: Icons.ContextMenu,
          }),
        },
        m(MenuItem, {
          label: 'Query',
          onclick: () => copyToClipboard(query),
        }),
        resp &&
          resp.error === undefined && [
            m(MenuItem, {
              label: 'Result (.tsv)',
              onclick: () => queryResponseAsTsvToClipboard(resp),
            }),
            m(MenuItem, {
              label: 'Result (.md)',
              onclick: () => queryResponseAsMarkdownToClipboard(resp),
            }),
          ],
      ),
    ];
  }

  renderTableContent(resp: QueryResponse) {
    return m(
      '.pf-query-panel',
      resp.statementWithOutputCount > 1 &&
        m(
          '.pf-query-warning',
          m(
            Callout,
            {icon: 'warning'},
            `${resp.statementWithOutputCount} out of ${resp.statementCount} `,
            'statements returned a result. ',
            'Only the results for the last statement are displayed.',
          ),
        ),
      m(QueryTableContent, {trace: this.trace, resp}),
    );
  }
}

async function queryResponseAsTsvToClipboard(
  resp: QueryResponse,
): Promise<void> {
  const lines: string[][] = [];
  lines.push(resp.columns);
  for (const row of resp.rows) {
    const line = [];
    for (const col of resp.columns) {
      const value = row[col];
      line.push(value === null ? 'NULL' : `${value}`);
    }
    lines.push(line);
  }
  await copyToClipboard(lines.map((line) => line.join('\t')).join('\n'));
}

async function queryResponseAsMarkdownToClipboard(
  resp: QueryResponse,
): Promise<void> {
  // Convert all values to strings.
  // rows = [header, separators, ...body]
  const rows: string[][] = [];
  rows.push(resp.columns);
  rows.push(resp.columns.map((_) => '---'));
  for (const responseRow of resp.rows) {
    rows.push(
      resp.columns.map((responseCol) => {
        const value = responseRow[responseCol];
        return value === null ? 'NULL' : `${value}`;
      }),
    );
  }

  // Find the maximum width of each column.
  const maxWidths: number[] = Array(resp.columns.length).fill(0);
  for (const row of rows) {
    for (let i = 0; i < resp.columns.length; i++) {
      if (row[i].length > maxWidths[i]) {
        maxWidths[i] = row[i].length;
      }
    }
  }

  const text = rows
    .map((row, rowIndex) => {
      // Pad each column to the maximum width with hyphens (separator row) or
      // spaces (all other rows).
      const expansionChar = rowIndex === 1 ? '-' : ' ';
      const line: string[] = row.map(
        (str, colIndex) =>
          str + expansionChar.repeat(maxWidths[colIndex] - str.length),
      );
      return `| ${line.join(' | ')} |`;
    })
    .join('\n');

  await copyToClipboard(text);
}
