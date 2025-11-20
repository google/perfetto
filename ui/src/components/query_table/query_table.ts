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
import {QueryResponse} from './queries';
import {Row} from '../../trace_processor/query_result';
import {Callout} from '../../widgets/callout';
import {DetailsShell} from '../../widgets/details_shell';
import {Router} from '../../core/router';
import {Trace} from '../../public/trace';
import {Icons} from '../../base/semantic_icons';
import {
  DataGrid,
  renderCell,
  DataGridApi,
} from '../widgets/data_grid/data_grid';
import {DataGridDataSource} from '../widgets/data_grid/common';
import {InMemoryDataSource} from '../widgets/data_grid/in_memory_data_source';
import {Anchor} from '../../widgets/anchor';
import {Box} from '../../widgets/box';
import {DataGridExportButton} from '../widgets/data_grid/export_buttons';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';

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
  readonly fillHeight: boolean;
}

export class QueryTable implements m.ClassComponent<QueryTableAttrs> {
  private readonly trace: Trace;
  private dataSource?: DataGridDataSource;
  private dataGridApi?: DataGridApi;

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
    const {resp, query, contextButtons = [], fillHeight} = attrs;

    return m(
      DetailsShell,
      {
        className: 'pf-query-table',
        title: this.renderTitle(resp),
        description: query,
        buttons: this.renderButtons(query, contextButtons),
        fillHeight,
      },
      resp && this.dataSource && this.renderTableContent(resp, this.dataSource),
    );
  }

  private renderTitle(resp?: QueryResponse) {
    if (!resp) {
      return 'Query - running';
    }
    const result = resp.error ? 'error' : `${resp.rows.length} rows`;
    return `Query result (${result}) - ${resp.durationMs.toLocaleString()}ms`;
  }

  private renderButtons(query: string, contextButtons: m.Child[]) {
    return [
      contextButtons,
      m(CopyToClipboardButton, {
        textToCopy: query,
        title: 'Copy executed query to clipboard',
        label: 'Copy Query',
      }),
      this.dataGridApi && m(DataGridExportButton, {api: this.dataGridApi}),
    ];
  }

  private renderTableContent(
    resp: QueryResponse,
    dataSource: DataGridDataSource,
  ) {
    return m(
      '.pf-query-panel',
      resp.statementWithOutputCount > 1 &&
        m(Box, [
          m(Callout, {icon: 'warning'}, [
            `${resp.statementWithOutputCount} out of ${resp.statementCount} `,
            'statements returned a result. ',
            'Only the results for the last statement are displayed.',
          ]),
        ]),
      this.renderContent(resp, dataSource),
    );
  }

  private renderContent(resp: QueryResponse, dataSource: DataGridDataSource) {
    if (resp.error) {
      return m('.pf-query-panel__query-error', `SQL error: ${resp.error}`);
    }

    const onTimelinePage =
      Router.parseUrl(window.location.href).page === '/viewer';

    return m(DataGrid, {
      // If filters are defined by no onFilterChanged handler, the grid operates
      // in filter read only mode.
      fillHeight: true,
      filters: [],
      columns: resp.columns.map((c) => ({name: c})),
      data: dataSource,
      onReady: (api) => {
        this.dataGridApi = api;
      },
      cellRenderer: (value, name, row) => {
        const sliceId = getSliceId(row);
        const cell = renderCell(value, name);
        if (
          name === 'id' &&
          sliceId !== undefined &&
          onTimelinePage &&
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
