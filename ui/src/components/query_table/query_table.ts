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
import {Trace} from '../../public/trace';
import {Icons} from '../../base/semantic_icons';
import {DataGrid, renderCell, DataGridApi} from '../widgets/datagrid/datagrid';
import {
  CellRenderer,
  ColumnSchema,
  SchemaRegistry,
} from '../widgets/datagrid/datagrid_schema';
import {InMemoryDataSource} from '../widgets/datagrid/in_memory_data_source';
import {Anchor} from '../../widgets/anchor';
import {Box} from '../../widgets/box';
import {DataGridExportButton} from '../widgets/datagrid/export_button';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import {DataSource} from '../widgets/datagrid/data_source';
import {AddDebugTrackMenu} from '../tracks/add_debug_track_menu';
import {Button} from '../../widgets/button';
import {PopupMenu} from '../../widgets/menu';
import {PopupPosition} from '../../widgets/popup';
import {exists} from '../../base/utils';
import {EmptyState} from '../../widgets/empty_state';

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

interface QueryResultsTableAttrs {
  // The trace that the query was executed against - used for adding debug
  // tracks, navigating to slices, etc.
  readonly trace: Trace;

  // The executed query to display above the table and made available to copy.
  readonly query?: string;

  // If true, a loading indicator is shown.
  readonly isLoading?: boolean;

  // The query response to display in the table.
  readonly resp?: QueryResponse;

  // If true, the table will expand to fill the height of its container.
  readonly fillHeight: boolean;

  // What to render in the body when there is no response. If undefined, a
  // default "No results" empty state is shown.
  readonly emptyState?: m.Children;
}

/**
 * A component that displays the results of a query in a DataGrid with a nicely
 * styled toolbar and additional buttons for copying the query, exporting the
 * data and creating a debug track.
 */
export class QueryResultsTable
  implements m.ClassComponent<QueryResultsTableAttrs>
{
  private dataSource?: DataSource;
  private dataGridApi?: DataGridApi;

  constructor({attrs}: m.CVnode<QueryResultsTableAttrs>) {
    if (attrs.resp) {
      this.dataSource = new InMemoryDataSource(attrs.resp.rows);
    }
  }

  onbeforeupdate(
    vnode: m.Vnode<QueryResultsTableAttrs, this>,
    old: m.VnodeDOM<QueryResultsTableAttrs, this>,
  ): boolean | void {
    if (vnode.attrs.resp !== old.attrs.resp) {
      if (vnode.attrs.resp) {
        this.dataSource = new InMemoryDataSource(vnode.attrs.resp.rows);
      } else {
        this.dataSource = undefined;
      }
    }
  }

  view({attrs}: m.CVnode<QueryResultsTableAttrs>) {
    const {resp, query, fillHeight, trace, isLoading, emptyState} = attrs;

    return m(
      DetailsShell,
      {
        className: 'pf-query-table',
        title: this.renderTitle(isLoading, resp),
        description: query,
        buttons: this.renderButtons(trace, query, resp),
        fillHeight,
      },
      this.renderBody(trace, resp, isLoading, emptyState),
    );
  }

  private renderBody(
    trace: Trace,
    resp: QueryResponse | undefined,
    isLoading?: boolean,
    emptyState?: m.Children,
  ) {
    if (isLoading) {
      return m(EmptyState, {
        fillHeight: true,
        title: 'Query running...',
        icon: 'pending',
      });
    }

    if (!resp) {
      if (emptyState !== undefined) {
        return emptyState;
      }
      return m(EmptyState, {
        fillHeight: true,
        title: 'No results',
      });
    }

    if (!this.dataSource) {
      return null;
    }

    return this.renderTableContent(trace, resp, this.dataSource);
  }

  private renderTitle(isLoading?: boolean, resp?: QueryResponse) {
    if (isLoading) {
      return 'Query Results - running...';
    }

    if (resp === undefined) {
      return 'Query Results - empty';
    }

    const result = resp.error ? 'error' : `${resp.rows.length} rows`;
    return `Query Results (${result}) - ${resp.durationMs.toLocaleString()}ms`;
  }

  private renderButtons(
    trace: Trace,
    query: string | undefined,
    resp: QueryResponse | undefined,
  ) {
    return [
      this.renderAddDebugTrackButton(trace, resp),
      query &&
        m(CopyToClipboardButton, {
          textToCopy: query,
          title: 'Copy executed query to clipboard',
          label: 'Copy Query',
        }),
      this.dataGridApi &&
        m(DataGridExportButton, {onExportData: this.dataGridApi.exportData}),
    ];
  }

  private renderAddDebugTrackButton(
    trace: Trace,
    resp: QueryResponse | undefined,
  ) {
    if (!resp || resp.error || !exists(resp.lastStatementSql)) {
      return null;
    }

    return m(
      PopupMenu,
      {
        trigger: m(Button, {label: 'Add debug track'}),
        position: PopupPosition.Top,
      },
      m(AddDebugTrackMenu, {
        trace,
        query: resp.lastStatementSql,
        availableColumns: resp.columns,
      }),
    );
  }

  private renderTableContent(
    trace: Trace,
    resp: QueryResponse,
    dataSource: DataSource,
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
      this.renderContent(trace, resp, dataSource),
    );
  }

  private renderContent(
    trace: Trace,
    resp: QueryResponse,
    dataSource: DataSource,
  ) {
    if (resp.error) {
      return m('.pf-query-panel__query-error', `SQL error: ${resp.error}`);
    }

    // Build schema directly
    const columnSchema: ColumnSchema = {};
    for (const column of resp.columns) {
      const cellRenderer: CellRenderer | undefined =
        column === 'id'
          ? (value, row) => {
              const sliceId = getSliceId(row);
              const cell = renderCell(value, column);
              if (sliceId !== undefined && isSliceish(row)) {
                return m(
                  Anchor,
                  {
                    title: 'Go to slice',
                    icon: Icons.UpdateSelection,
                    onclick: () => this.goToSlice(trace, sliceId, false),
                    ondblclick: () => this.goToSlice(trace, sliceId, true),
                  },
                  cell,
                );
              } else {
                return renderCell(value, column);
              }
            }
          : undefined;

      columnSchema[column] = {cellRenderer};
    }

    const schema: SchemaRegistry = {data: columnSchema};

    return m(DataGrid, {
      schema,
      rootSchema: 'data',
      initialColumns: resp.columns.map((col) => ({field: col})),
      // If filters are defined by no onFilterChanged handler, the grid operates
      // in filter read only mode.
      fillHeight: true,
      filters: [],
      data: dataSource,
      onReady: (api) => {
        this.dataGridApi = api;
      },
    });
  }

  private goToSlice(
    trace: Trace,
    sliceId: number,
    switchToCurrentSelectionTab: boolean,
  ): void {
    // Navigate to the timeline page
    trace.navigate('#!/viewer');
    trace.selection.selectSqlEvent('slice', sliceId, {
      switchToCurrentSelectionTab,
      scrollToSelection: true,
    });
  }
}
