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
import {Button} from '../../widgets/button';
import {Callout} from '../../widgets/callout';
import {DetailsShell} from '../../widgets/details_shell';
import {Router} from '../../core/router';
import {AppImpl} from '../../core/app_impl';
import {Trace} from '../../public/trace';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {Icons} from '../../base/semantic_icons';
import {DataGrid, renderCell} from '../widgets/data_grid/data_grid';
import {DataGridDataSource} from '../widgets/data_grid/common';
import {InMemoryDataSource} from '../widgets/data_grid/in_memory_data_source';
import {Anchor} from '../../widgets/anchor';

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

interface QueryTableAttrs {
  readonly trace: Trace;
  readonly query: string;
  readonly resp?: QueryResponse;
  readonly contextButtons?: m.Child[];
  readonly fillParent: boolean;
}

export class QueryTable implements m.ClassComponent<QueryTableAttrs> {
  private readonly trace: Trace;
  private dataSource?: DataGridDataSource;

  constructor({attrs}: m.CVnode<QueryTableAttrs>) {
    this.trace = attrs.trace;
    if (attrs.resp) {
      this.dataSource = new InMemoryDataSource(attrs.resp.rows);
    }
  }

  onbeforeupdate(
    vnode: m.Vnode<QueryTableAttrs, this>,
    old: m.VnodeDOM<QueryTableAttrs, this>,
  ): boolean | void {
    if (vnode.attrs.resp !== old.attrs.resp) {
      if (vnode.attrs.resp) {
        this.dataSource = new InMemoryDataSource(vnode.attrs.resp.rows);
      } else {
        this.dataSource = undefined;
      }
    }
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
      resp && this.dataSource && this.renderTableContent(resp, this.dataSource),
    );
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

  private renderTableContent(
    resp: QueryResponse,
    dataSource: DataGridDataSource,
  ) {
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
      this.renderContent(resp, dataSource),
    );
  }

  private renderContent(resp: QueryResponse, dataSource: DataGridDataSource) {
    if (resp.error) {
      return m('.query-error', `SQL error: ${resp.error}`);
    }

    const onViewerPage =
      Router.parseUrl(window.location.href).page === '/viewer';

    return m(DataGrid, {
      // If filters are defined by no onFilterChanged handler, the grid operates
      // in filter read only mode.
      filters: [],
      columns: resp.columns.map((c) => ({name: c})),
      dataSource,
      cellRenderer: (value, name, row) => {
        const sliceId = getSliceId(row);
        const cell = renderCell(value, name);
        if (
          name === 'id' &&
          sliceId !== undefined &&
          onViewerPage &&
          isSliceish(row)
        ) {
          return m(
            Anchor,
            {
              title: 'Go to slice',
              icon: Icons.UpdateSelection,
              onclick: () => this.goToSlice(sliceId, false),
              ondblclick: () => this.goToSlice(sliceId, true),
            },
            cell,
          );
        } else {
          return cell;
        }
      },
    });
  }

  private goToSlice(
    sliceId: number,
    switchToCurrentSelectionTab: boolean,
  ): void {
    this.trace.selection.selectSqlEvent('slice', sliceId, {
      switchToCurrentSelectionTab,
      scrollToSelection: true,
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
