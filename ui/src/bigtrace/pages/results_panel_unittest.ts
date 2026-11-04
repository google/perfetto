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

import {describe, expect, test, vi} from 'vitest';
import type {Vnode} from 'mithril';
import {ensureDefined} from '../../base/assert';
import {BigtraceAsyncDataSource} from '../query/bigtrace_async_data_source';
import type {BigtraceQueryClient} from '../query/bigtrace_query_client';
import {PollingController} from '../query/polling_controller';
import {queryStore} from '../query/query_store';
import {
  type BigTraceEditorTab,
  makeQueryResponse,
  QueryTabsState,
} from './query_tabs_state';
import {renderResultsPanel} from './results_panel';

const SQL = 'SELECT COUNT() AS count FROM slice;';

function vnodeText(node: unknown, out: string[] = []): string[] {
  if (node === null || node === undefined || typeof node === 'boolean') {
    return out;
  }
  if (typeof node === 'string' || typeof node === 'number') {
    out.push(String(node));
    return out;
  }
  const tag = (node as Vnode).tag;
  if (typeof tag === 'function' || typeof tag === 'object') {
    out.push(`<${(tag as {name?: string}).name ?? 'component'}>`);
  }
  for (const [key, value] of Object.entries(node as object)) {
    if (key === 'tag' || key === 'dom' || key === 'events') continue;
    if (typeof value === 'string') out.push(value);
    else if (typeof value === 'object' && value !== null) {
      vnodeText(value, out);
    }
  }
  return out;
}

interface FakeClientOptions {
  readonly status?: string;
  readonly phase?: string;
  readonly processedRows?: number;
  readonly tableName?: string;
}

function fakeClient(opts: FakeClientOptions = {}): BigtraceQueryClient {
  const status = opts.status ?? 'SUCCESS';
  const processedRows = opts.processedRows ?? 1;
  return {
    getStatus: vi.fn().mockResolvedValue({
      status,
      phase: opts.phase,
      processedRows,
      processedTraces: 1,
      totalTraces: 1,
    }),
    getQueryExecution: vi.fn().mockResolvedValue({
      status,
      phase: opts.phase,
      processedRows,
      startTime: '2026-01-01T00:00:00Z',
      endTime: '2026-01-01T00:00:01Z',
      tableName: opts.tableName,
      schema: [{name: 'count', type: 'INT64'}],
    }),
    fetchResults: vi.fn().mockResolvedValue({
      rows: [{count: 42n}],
      columns: ['count'],
      schema: [{name: 'count', type: 'INT64'}],
      totalFilteredRows: processedRows,
      availableColumnNames: ['count'],
    }),
  } as unknown as BigtraceQueryClient;
}

function runningTab(
  tabsState: QueryTabsState,
  uuid: string,
  client: BigtraceQueryClient,
): BigTraceEditorTab {
  const tab = ensureDefined(tabsState.tabs[0]);
  tab.configured = true;
  tab.editorText = SQL;
  tab.queryUuid = uuid;
  tab.queryClient = client;
  tab.isLoading = true;
  tab.execution = queryStore.getOrCreate(uuid, {
    perfettoSql: SQL,
    status: 'IN_PROGRESS',
  });
  tab.dataSource = new BigtraceAsyncDataSource(
    uuid,
    client,
    () => tab.execution?.processedRows ?? 0,
    tab.lifecycle.signal,
    () => tab.execution?.schema,
  );
  tab.queryResult = makeQueryResponse(SQL);
  return tab;
}

async function flushPolling(
  controller: PollingController,
  tab: BigTraceEditorTab,
): Promise<void> {
  controller.start(tab);
  for (let i = 0; i < 6; i++) {
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
  controller.stop(tab);
}

describe('results panel across preview and full-table phases', () => {
  test('shows the results grid when the F1 table completes', async () => {
    const tabsState = new QueryTabsState();
    const client = fakeClient({tableName: 'a_table'});
    const tab = runningTab(tabsState, 'uuid-with-table', client);
    const controller = new PollingController({
      redraw: () => {},
      onHistoryChanged: () => {},
    });

    await flushPolling(controller, tab);

    const text = vnodeText(renderResultsPanel(tab, tabsState));
    expect(text).toContain('<DataGrid>');
    expect(text).not.toContain('Results no longer available');
  });

  test('reports a dropped result table when tableName is absent and rows > 0', async () => {
    const tabsState = new QueryTabsState();
    const client = fakeClient({tableName: undefined, processedRows: 1});
    const tab = runningTab(tabsState, 'uuid-no-table', client);
    const controller = new PollingController({
      redraw: () => {},
      onHistoryChanged: () => {},
    });

    await flushPolling(controller, tab);

    const text = vnodeText(renderResultsPanel(tab, tabsState));
    expect(text).toContain('Results no longer available');
  });

  test('reports zero rows when a query succeeds with zero rows and no table', async () => {
    const tabsState = new QueryTabsState();
    const client = fakeClient({tableName: undefined, processedRows: 0});
    const tab = runningTab(tabsState, 'uuid-zero-rows', client);
    const controller = new PollingController({
      redraw: () => {},
      onHistoryChanged: () => {},
    });

    await flushPolling(controller, tab);

    const text = vnodeText(renderResultsPanel(tab, tabsState));
    expect(text).toContain('Query returned no rows');
    expect(text).not.toContain('Results no longer available');
  });

  test('refreshes the same BigtraceAsyncDataSource for preview (FULL_TABLE) and full table (SUCCESS)', async () => {
    const tabsState = new QueryTabsState();
    const client = fakeClient({
      status: 'IN_PROGRESS',
      phase: 'FULL_TABLE',
      processedRows: 1000,
      tableName: 'same_table',
    });
    const tab = runningTab(tabsState, 'uuid-two-phase', client);
    const controller = new PollingController({
      redraw: () => {},
      onHistoryChanged: () => {},
    });

    await flushPolling(controller, tab);

    expect(client.fetchResults).toHaveBeenCalledTimes(1);
    const previewText = vnodeText(renderResultsPanel(tab, tabsState));
    expect(previewText).toContain('<DataGrid>');
    expect(previewText).toContain(
      'Previewing 1,000 rows (building full table…)',
    );

    // Phase 2 finishes on the same table: status transitions to SUCCESS.
    (client.getStatus as ReturnType<typeof vi.fn>).mockResolvedValue({
      status: 'SUCCESS',
      processedRows: 5000,
      processedTraces: 10,
      totalTraces: 10,
    });
    (client.getQueryExecution as ReturnType<typeof vi.fn>).mockResolvedValue({
      status: 'SUCCESS',
      processedRows: 5000,
      startTime: '2026-01-01T00:00:00Z',
      endTime: '2026-01-01T00:00:05Z',
      tableName: 'same_table',
      schema: [{name: 'count', type: 'INT64'}],
    });
    (client.fetchResults as ReturnType<typeof vi.fn>).mockResolvedValue({
      rows: [{count: 42n}],
      columns: ['count'],
      schema: [{name: 'count', type: 'INT64'}],
      totalFilteredRows: 5000,
      availableColumnNames: ['count'],
    });

    await flushPolling(controller, tab);

    expect(client.fetchResults).toHaveBeenCalledTimes(2);
    const finalText = vnodeText(renderResultsPanel(tab, tabsState));
    expect(finalText).toContain('Showing 5,000 rows');
  });
});
