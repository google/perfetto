// Copyright (C) 2026 The Android Open Source Project
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
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import {EmptyState} from '../../widgets/empty_state';
import {linkify} from '../../widgets/anchor';
import {Spinner} from '../../widgets/spinner';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import type {DataSource} from '../../components/widgets/datagrid/data_source';
import type {
  ColumnSchema,
  SchemaRegistry,
} from '../../components/widgets/datagrid/datagrid_schema';
import type {
  Column,
  SortDirection,
} from '../../components/widgets/datagrid/model';
import {LINK_COLUMN, resolveResultColumns} from '../settings/column_order';
import {BigtraceAsyncDataSource} from '../query/bigtrace_async_data_source';
import {TERMINAL_STATUSES} from '../query/query_store';
import type {
  BigTraceEditorTab,
  QueryResponse,
  QueryTabsState,
} from './query_tabs_state';
import {formatDurationS} from './status_box';

// Per-tab results sort. DataGrid carries sort on the Column object, so
// controlled-mode `columns` must splice it back each render or a header click
// is dropped on our redraws. Not persisted, unlike the visible-columns set.
const resultsSortByTab = new WeakMap<
  BigTraceEditorTab,
  {field: string; direction: SortDirection}
>();

export function renderResultsGrid(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
): m.Children {
  const queryResult = tab.queryResult!;
  const dataSource = tab.dataSource!;

  const isInitialLoad =
    tab.queryUuid !== undefined &&
    tab.queryUuid !== '' &&
    (tab.execution === undefined || tab.execution.status === 'UNKNOWN');
  if (isInitialLoad) {
    return m(
      EmptyState,
      {
        title: 'Loading query status...',
        icon: 'hourglass_empty',
        fillHeight: true,
      },
      m(Spinner),
    );
  }

  const isTerminal =
    tab.execution?.status !== undefined &&
    TERMINAL_STATUSES.has(tab.execution.status);

  const tableContent: m.Children[] = [];

  let columns = queryResult.columns;
  if (columns.length === 0 && dataSource instanceof BigtraceAsyncDataSource) {
    columns = dataSource.getColumns() ?? [];
  }

  if (dataSource instanceof BigtraceAsyncDataSource) {
    const error = dataSource.getError();
    // A 400 before the query reaches a terminal state usually means the results
    // aren't ready yet, not a real failure — keep the loading state until the
    // run finishes. A 400 after completion (e.g. a bad filter) or any non-400
    // error is a real error worth surfacing.
    const isRealError =
      error !== null &&
      error !== '' &&
      (isTerminal || dataSource.getErrorStatus() !== 400);
    if (isRealError) {
      // Render in a selectable <pre> (not an EmptyState, whose text is
      // truncated and user-select:none) so the full message is readable and
      // copyable.
      tableContent.push(
        m('.pf-bt-results-error', [
          m('.pf-bt-results-error__title', 'Failed to load results'),
          m('pre.pf-bt-error-content', error),
        ]),
      );
      return tableContent;
    }
  }

  if (columns.length === 0) {
    dataSource.useRows({mode: 'flat', columns: []});
    tableContent.push(
      m(
        EmptyState,
        {
          title: 'Loading schema...',
          icon: 'hourglass_empty',
          fillHeight: true,
        },
        m(Spinner),
      ),
    );
    return tableContent;
  }

  tableContent.push(
    renderDataGrid(tab, tabsState, columns, queryResult, dataSource),
  );
  return tableContent;
}

function renderDataGrid(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
  columns: ReadonlyArray<string>,
  queryResult: QueryResponse,
  dataSource: DataSource,
): m.Children {
  // "+ Add column" choices. Async: the full union (result ∪ metadata) from
  // availableColumnNames. Sync / pre-fetch: fall back to the result columns.
  let allColumns: ReadonlyArray<string> = columns;
  if (dataSource instanceof BigtraceAsyncDataSource) {
    const available = dataSource.availableColumnNames;
    if (available !== undefined && available.length > 0) {
      allColumns = available;
    }
  }

  const columnSchema: ColumnSchema = {};
  for (const column of allColumns) {
    if (column === LINK_COLUMN) {
      columnSchema[column] = {
        cellRenderer: (value) => {
          if (value === null || value === undefined) return '';
          return linkify(String(value));
        },
      };
    } else {
      columnSchema[column] = {cellRenderer: undefined};
    }
  }
  const schema: SchemaRegistry = {data: columnSchema};

  // Per-tab visible subset (empty/unset → defaults); shipped as the
  // `:fetch_results` `columns` projection.
  const visible = resolveResultColumns(tab.resultColumns, allColumns);
  const defaultVisible = resolveResultColumns(null, allColumns);
  const isAsync = dataSource instanceof BigtraceAsyncDataSource;
  const sortState = resultsSortByTab.get(tab);

  return m(DataGrid, {
    schema,
    rootSchema: 'data',
    disablePivotControls: true,
    // Splice per-tab sort onto its column so a header click survives redraws.
    columns: visible.map((col) => {
      const base: Column = {id: col, field: col};
      if (sortState && sortState.field === col) {
        return {...base, sort: sortState.direction};
      }
      return base;
    }),
    onColumnsChanged: (cols: ReadonlyArray<Column>) => {
      // Stash sort before collapsing to string[], else it's lost next render.
      const sorted = cols.find((c) => c.sort);
      if (sorted && sorted.sort !== undefined) {
        resultsSortByTab.set(tab, {
          field: sorted.field,
          direction: sorted.sort,
        });
      } else {
        resultsSortByTab.delete(tab);
      }
      const nextColumns = cols.map((c) => c.field);
      tab.resultColumns = nextColumns.length === 0 ? null : nextColumns;
      tabsState.markDirty();
    },
    canAddColumns: true,
    canRemoveColumns: true,
    className: 'pf-bt-query-page__results',
    data: dataSource,
    fillHeight: true,
    showExportButton: true,
    emptyStateMessage:
      isAsync && visible.length >= defaultVisible.length
        ? 'Query returned no rows'
        : 'No rows match the visible columns',
    toolbarItemsLeft: [
      m('span.pf-bt-results-summary', renderResultsSummary(tab, queryResult)),
    ],
    toolbarItemsRight: [
      m(CopyToClipboardButton, {
        textToCopy: queryResult.query,
        title: 'Copy executed query to clipboard',
        label: 'Copy Query',
      }),
    ],
  });
}

function renderResultsSummary(
  tab: BigTraceEditorTab,
  queryResult: QueryResponse,
): string {
  if (!tab.materialize) {
    const durationStr = formatDurationS(Math.max(0, queryResult.durationMs));
    return `Returned ${queryResult.totalRowCount.toLocaleString()} rows in ${durationStr}`;
  }
  const asyncDs =
    tab.dataSource instanceof BigtraceAsyncDataSource
      ? tab.dataSource
      : undefined;
  const count = asyncDs?.filteredTotalRows ?? tab.execution?.processedRows ?? 0;
  const isTerminal =
    tab.execution?.status !== undefined &&
    TERMINAL_STATUSES.has(tab.execution.status);
  const text = `${count.toLocaleString()} rows`;
  return isTerminal ? text : `${text} · running…`;
}
