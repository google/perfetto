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
import {BigtraceAsyncDataSource} from './bigtrace_async_data_source';
import {BigtraceQueryClient} from './bigtrace_query_client';
import {QueryRunner} from './query_runner';
import {queryStore} from './query_store';
import {QueryTabsState} from '../pages/query_tabs_state';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import {endpointStorage} from '../settings/endpoint_storage';

describe('QueryRunner', () => {
  test('rehydrateTab loads a finished execution without starting polling', async () => {
    const getExecSpy = vi
      .spyOn(BigtraceQueryClient.prototype, 'getQueryExecution')
      .mockResolvedValue({
        status: 'SUCCESS',
        processedRows: 10,
        processedTraces: 100,
        totalTraces: 100,
        startTime: '2026-01-01T00:00:00Z',
        endTime: '2026-01-01T00:04:25Z',
        tableName: 'saved_table',
        schema: [{name: 'dur', type: 'INT64'}],
      });
    const fetchSpy = vi
      .spyOn(BigtraceQueryClient.prototype, 'fetchResults')
      .mockResolvedValue({
        rows: [{dur: 5}],
        columns: ['dur'],
        schema: [{name: 'dur', type: 'INT64'}],
        totalFilteredRows: 10,
        availableColumnNames: ['dur'],
      });
    try {
      const tabsState = new QueryTabsState();
      const tab = tabsState.tabs[0]!;
      tab.editorText = 'SELECT dur FROM slice;';
      tab.queryUuid = 'rehydrate-success-uuid';

      const runner = new QueryRunner({
        onHistoryChanged: () => {},
        redraw: () => {},
      });
      await runner.rehydrateTab(tab);

      expect(tab.isLoading).toBe(false);
      expect(tab.pollInterval).toBeUndefined();
      expect(tab.queryResult?.durationMs).toBe(265_000);
      expect(fetchSpy).toHaveBeenCalledTimes(1);
      expect(getExecSpy).toHaveBeenCalledTimes(1);
    } finally {
      getExecSpy.mockRestore();
      fetchSpy.mockRestore();
    }
  });

  test('run resets prior execution state and column visibility', async () => {
    const tabsState = new QueryTabsState();
    const tab = tabsState.tabs[0]!;
    tab.queryUuid = 'old-uuid';
    tab.execution = queryStore.getOrCreate('old-uuid', {status: 'SUCCESS'});
    tab.resultColumns = ['old_col'];
    tab.resultsTabKey = 'error';

    // With no endpoint configured, run() resets upfront and surfaces the setup
    // prompt synchronously.
    const runner = new QueryRunner({
      onHistoryChanged: () => {},
      redraw: () => {},
    });
    await runner.run(tab, 'SELECT 1;');

    expect(tab.queryUuid).toBeUndefined();
    expect(tab.execution).toBeUndefined();
    expect(tab.resultColumns).toBeNull();
    expect(tab.resultsTabKey).toBeUndefined();
  });

  test('run notifies onHistoryChanged once executeAsync returns queryUuid', async () => {
    const prevEndpoint = endpointStorage.get('bigtraceEndpoint')?.get() ?? '';
    endpointStorage.get('bigtraceEndpoint')?.set('https://example.com/v1');
    const loadSettingsSpy = vi
      .spyOn(bigTraceSettingsStorage, 'loadSettings')
      .mockResolvedValue();
    const schemaSpy = vi
      .spyOn(BigtraceQueryClient.prototype, 'listTraceMetadataSchema')
      .mockResolvedValue({columns: []});
    const execAsyncSpy = vi
      .spyOn(BigtraceQueryClient.prototype, 'executeAsync')
      .mockResolvedValue({
        queryUuid: 'new-async-uuid',
        rows: [],
        columns: [],
        schema: [],
      });
    const getExecSpy = vi
      .spyOn(BigtraceQueryClient.prototype, 'getQueryExecution')
      .mockResolvedValue({
        status: 'IN_PROGRESS',
        phase: 'PREVIEW',
        processedRows: 0,
      });
    const getStatusSpy = vi
      .spyOn(BigtraceQueryClient.prototype, 'getStatus')
      .mockResolvedValue({
        status: 'IN_PROGRESS',
        phase: 'PREVIEW',
        processedRows: 0,
      });
    try {
      const tabsState = new QueryTabsState();
      const tab = tabsState.tabs[0]!;
      const uuidsAtRefresh: Array<string | undefined> = [];
      const runner = new QueryRunner({
        onHistoryChanged: () => uuidsAtRefresh.push(tab.queryUuid),
        redraw: () => {},
      });
      await runner.run(tab, 'SELECT 1;');
      clearInterval(tab.pollInterval);
      expect(uuidsAtRefresh).toEqual([undefined, 'new-async-uuid']);
    } finally {
      endpointStorage.get('bigtraceEndpoint')?.set(prevEndpoint);
      loadSettingsSpy.mockRestore();
      schemaSpy.mockRestore();
      execAsyncSpy.mockRestore();
      getExecSpy.mockRestore();
      getStatusSpy.mockRestore();
    }
  });

  test('cancel refreshes execution details so a race with SUCCESS preserves server state', async () => {
    const uuid = 'cancel-race-uuid';
    const client = {
      cancelQuery: vi.fn().mockResolvedValue(undefined),
      getQueryExecution: vi.fn().mockResolvedValue({
        status: 'SUCCESS',
        processedRows: 500,
        processedTraces: 100,
        totalTraces: 100,
        tableName: 'final_table',
        schema: [{name: 'dur', type: 'INT64'}],
      }),
    } as unknown as BigtraceQueryClient;
    const ds = new BigtraceAsyncDataSource(uuid, client, () => 500);
    const refreshSpy = vi.spyOn(ds, 'refresh').mockResolvedValue();

    const tabsState = new QueryTabsState();
    const tab = tabsState.tabs[0]!;
    tab.queryUuid = uuid;
    tab.queryClient = client;
    tab.dataSource = ds;
    tab.isLoading = true;
    tab.execution = queryStore.getOrCreate(uuid, {
      status: 'IN_PROGRESS',
      phase: 'FULL_TABLE',
      processedRows: 100,
    });

    const runner = new QueryRunner({
      onHistoryChanged: () => {},
      redraw: () => {},
    });
    await runner.cancel(tab);

    expect(tab.execution?.status).toBe('SUCCESS');
    expect(tab.execution?.phase).toBeUndefined();
    expect(tab.execution?.tableName).toBe('final_table');
    expect(refreshSpy).toHaveBeenCalledTimes(1);
  });
});
