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
import {BigtraceAsyncDataSource} from '../query/bigtrace_async_data_source';
import type {BigtraceQueryClient} from '../query/bigtrace_query_client';
import {PollingController} from '../query/polling_controller';
import {queryStore} from '../query/query_store';
import {
  type BigTraceEditorTab,
  QueryTabsState,
  makeQueryResponse,
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

function fakeClient(tableName?: string): BigtraceQueryClient {
  return {
    getStatus: vi.fn().mockResolvedValue({
      status: 'SUCCESS',
      processedRows: 1,
      processedTraces: 1,
      totalTraces: 1,
    }),
    getQueryExecution: vi.fn().mockResolvedValue({
      status: 'SUCCESS',
      processedRows: 1,
      startTime: '2026-01-01T00:00:00Z',
      endTime: '2026-01-01T00:00:01Z',
      tableName,
      schema: [{name: 'count', type: 'INT64'}],
    }),
    fetchResults: vi.fn().mockResolvedValue({
      rows: [{count: 42}],
      columns: ['count'],
      schema: [{name: 'count', type: 'INT64'}],
      totalFilteredRows: 1,
      availableColumnNames: ['count'],
    }),
  } as unknown as BigtraceQueryClient;
}

function runningTab(
  tabsState: QueryTabsState,
  uuid: string,
  client: BigtraceQueryClient,
): BigTraceEditorTab {
  const tab = tabsState.tabs[0];
  tab.materialize = true;
  tab.configured = true;
  tab.editorText = SQL;
  tab.queryUuid = uuid;
  tab.queryClient = client;
  tab.isLoading = true;
  tab.execution = queryStore.getOrCreate(uuid, {
    perfettoSql: SQL,
    materialized: true,
    status: 'IN_PROGRESS',
  });
  tab.dataSource = new BigtraceAsyncDataSource(
    uuid,
    client,
    () => tab.execution?.processedRows ?? 0,
    tab.lifecycle.signal,
    () => tab.execution?.schema,
  );
  tab.queryResult = makeQueryResponse(SQL, {durationMs: 1});
  return tab;
}

async function pollToCompletion(tab: BigTraceEditorTab): Promise<void> {
  new PollingController({
    redraw: () => {},
    onHistoryChanged: () => {},
  }).start(tab);
  for (let i = 0; i < 5; i++) {
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
}

describe('results panel after a persistent query completes', () => {
  test('shows the results grid without the history sidebar', async () => {
    const tabsState = new QueryTabsState();
    const tab = runningTab(tabsState, 'uuid-with-table', fakeClient('a_table'));

    await pollToCompletion(tab);

    const text = vnodeText(renderResultsPanel(tab, tabsState));
    expect(text).toContain('<DataGrid>');
    expect(text).not.toContain('Results no longer available');
  });

  test('reports a dropped result table when tableName is absent', async () => {
    const tabsState = new QueryTabsState();
    const tab = runningTab(tabsState, 'uuid-no-table', fakeClient(undefined));

    await pollToCompletion(tab);

    const text = vnodeText(renderResultsPanel(tab, tabsState));
    expect(text).toContain('Results no longer available');
  });
});
