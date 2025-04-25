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

// Controls how many rows we see per page when showing paginated results.
const ROWS_PER_PAGE = 50;

interface QueryTableRowAttrs {
  readonly trace: Trace;
  readonly row: Row;
  readonly columns: ReadonlyArray<string>;
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
  readonly trace: Trace;
  readonly columns: ReadonlyArray<string>;
  readonly rows: ReadonlyArray<Row>;
}

class QueryTableContent implements m.ClassComponent<QueryTableContentAttrs> {
  view({attrs}: m.CVnode<QueryTableContentAttrs>) {
    const cols = [];
    for (const col of attrs.columns) {
      cols.push(m('td', col));
    }
    const tableHeader = m('tr', cols);

    const rows = attrs.rows.map((row) => {
      return m(QueryTableRow, {
        trace: attrs.trace,
        row,
        columns: attrs.columns,
      });
    });

    return m('table.pf-query-table', m('thead', tableHeader), m('tbody', rows));
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
  private pageNumber = 0;

  constructor({attrs}: m.CVnode<QueryTableAttrs>) {
    this.trace = attrs.trace;
  }

  view({attrs}: m.CVnode<QueryTableAttrs>) {
    const {resp, query, contextButtons = [], fillParent} = attrs;

    // Clamp the page number to ensure the page count doesn't exceed the number
    // of rows in the results.
    if (resp) {
      const pageCount = this.getPageCount(resp.rows.length);
      if (this.pageNumber >= pageCount) {
        this.pageNumber = Math.max(0, pageCount - 1);
      }
    } else {
      this.pageNumber = 0;
    }

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

  private getPageCount(rowCount: number) {
    return Math.floor((rowCount - 1) / ROWS_PER_PAGE) + 1;
  }

  private getFirstRowInPage() {
    return this.pageNumber * ROWS_PER_PAGE;
  }

  private getCountOfRowsInPage(totalRows: number) {
    const firstRow = this.getFirstRowInPage();
    const endStop = Math.min(firstRow + ROWS_PER_PAGE, totalRows);
    return endStop - firstRow;
  }

  private renderTitle(resp?: QueryResponse) {
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

  private renderButtons(
    query: string,
    contextButtons: m.Child[],
    resp?: QueryResponse,
  ) {
    return [
      resp && this.renderPrevNextButtons(resp),
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

  private renderPrevNextButtons(resp: QueryResponse) {
    const from = this.getFirstRowInPage();
    const to = Math.min(from + this.getCountOfRowsInPage(resp.rows.length)) - 1;
    const pageCount = this.getPageCount(resp.rows.length);

    return [
      `Showing rows ${from + 1} to ${to + 1} of ${resp.rows.length}`,
      m(Button, {
        label: 'Prev',
        icon: 'skip_previous',
        title: 'Go to previous page of results',
        disabled: this.pageNumber === 0,
        onclick: () => {
          this.pageNumber = Math.max(0, this.pageNumber - 1);
        },
      }),
      m(Button, {
        label: 'Next',
        icon: 'skip_next',
        title: 'Go to next page of results',
        disabled: this.pageNumber >= pageCount - 1,
        onclick: () => {
          this.pageNumber = Math.min(pageCount - 1, this.pageNumber + 1);
        },
      }),
    ];
  }

  private renderTableContent(resp: QueryResponse) {
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
      this.renderContent(resp),
    );
  }

  private renderContent(resp: QueryResponse) {
    if (resp.error) {
      return m('.query-error', `SQL error: ${resp.error}`);
    }

    // Pick out only the rows in this page.
    const rowOffset = this.getFirstRowInPage();
    const totalRows = this.getCountOfRowsInPage(resp.rows.length);
    const rowsInPage: Row[] = [];
    for (
      let rowIndex = rowOffset;
      rowIndex < rowOffset + totalRows;
      ++rowIndex
    ) {
      rowsInPage.push(resp.rows[rowIndex]);
    }

    return m(QueryTableContent, {
      trace: this.trace,
      columns: resp.columns,
      rows: rowsInPage,
    });
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
