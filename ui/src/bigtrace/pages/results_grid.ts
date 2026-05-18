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
import {DataSource} from '../../components/widgets/datagrid/data_source';
import {
  ColumnSchema,
  SchemaRegistry,
} from '../../components/widgets/datagrid/datagrid_schema';
import {SettingFilter} from '../settings/settings_types';
import {BigtraceAsyncDataSource} from '../query/bigtrace_async_data_source';
import {TERMINAL_STATUSES} from '../query/query_store';
import {QueryRunner} from '../query/query_runner';
import {
  BigTraceEditorTab,
  QueryResponse,
  QueryTabsState,
} from './query_tabs_state';
import {formatDurationS} from './status_box';

export function renderResultsGrid(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
  runner: QueryRunner,
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
    if (
      error !== null &&
      error !== '' &&
      (isTerminal || error.includes('status: 400') === false)
    ) {
      tableContent.push(
        m(EmptyState, {
          title: `Failed to load schema: ${error}`,
          icon: 'error',
          fillHeight: true,
        }),
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
    renderDataGrid(tab, tabsState, runner, columns, queryResult, dataSource),
  );
  return tableContent;
}

function renderDataGrid(
  tab: BigTraceEditorTab,
  _tabsState: QueryTabsState,
  _runner: QueryRunner,
  columns: ReadonlyArray<string>,
  queryResult: QueryResponse,
  dataSource: DataSource,
): m.Children {
  const querySettings: SettingFilter[] = tab.querySettings;

  const columnSchema: ColumnSchema = {};
  for (const column of columns) {
    if (column === 'link') {
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

  return m(DataGrid, {
    schema,
    rootSchema: 'data',
    enablePivotControls: false,
    initialColumns: columns
      .filter((col) => {
        if (!col.startsWith('_')) return true;
        if (col === '_trace_id') return true;
        const settingId = col.substring(1);
        return querySettings.some(
          (s) => s.settingId === settingId && s.category === 'TRACE_METADATA',
        );
      })
      .map((col) => ({id: col, field: col})),
    className: 'pf-query-page__results',
    data: dataSource,
    fillHeight: true,
    showExportButton: true,
    emptyStateMessage: 'Query returned no rows',
    toolbarItemsLeft: [
      m(
        'span.pf-query-page__results-summary',
        renderResultsSummary(tab, queryResult),
      ),
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
