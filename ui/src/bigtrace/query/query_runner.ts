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
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import {getBigtraceEndpoint} from '../settings/endpoint_storage';
import type {SettingFilter} from '../settings/settings_types';
import {effectiveQueryColumns} from '../settings/trace_selection_state';
import {BigtraceAsyncDataSource} from './bigtrace_async_data_source';
import {
  BigtraceQueryClient,
  type ExecuteOptions,
  QueryCancelledError,
  QueryNotFoundError,
} from './bigtrace_query_client';
import {forwardAbort} from './abort_utils';
import {
  isoToEpochMs,
  snapshotSettingsToFilters,
  type RawQueryExecution,
} from './query_history_storage';
import {queryStore, TERMINAL_STATUSES} from './query_store';
import {
  disabledSettingsFromSnapshot,
  effectiveTabSettings,
  makeQueryResponse,
} from '../pages/query_tabs_state';
import type {BigTraceEditorTab} from '../pages/query_tabs_state';
import {PollingController} from './polling_controller';

interface QueryRunnerCallbacks {
  readonly onHistoryChanged: () => void;
  readonly redraw?: () => void;
  readonly markDirty?: () => void;
}

// One instance per QueryPage; owns dispatch / cancel for each tab.
// Polling is delegated to PollingController.
export class QueryRunner {
  private readonly poller: PollingController;

  constructor(private readonly cb: QueryRunnerCallbacks) {
    this.poller = new PollingController({
      redraw: () => this.redraw(),
      onHistoryChanged: cb.onHistoryChanged,
    });
  }

  // Run `query` on `tab`. Aborts any in-flight query on the tab first.
  async run(tab: BigTraceEditorTab, query: string): Promise<void> {
    if (!query) return;

    tab.activeRequest?.abort();

    tab.isLoading = true;
    tab.queryResult = undefined;
    tab.lastProcessedRows = 0;
    tab.clientStartTime = Date.now();
    this.cb.markDirty?.();
    this.redraw();

    this.cb.onHistoryChanged();
    const endpoint = getBigtraceEndpoint();

    if (endpoint.trim() === '') {
      tab.queryResult = makeQueryResponse(query, {
        error: 'Set the BigTrace Endpoint in Settings before running queries.',
      });
      tab.dataSource = new InMemoryDataSource([]);
      tab.isLoading = false;
      this.redraw();
      return;
    }

    await bigTraceSettingsStorage.loadSettings();

    // Global defaults overridden by per-tab values, minus per-tab-disabled
    // settings. Same helper the trace grid uses, so /trace_metadata and
    // /execute_* stay in sync.
    const settings = effectiveTabSettings(tab);

    // Per-tab snapshot is the trace-selection source of truth: what the user
    // staged here (seeded from globals at tab creation, then edited). Globals
    // aren't re-read at run time.
    const traceFilters = tab.traceFilters;
    const traceOrderBy = tab.traceOrderBy;

    const queryClient = new BigtraceQueryClient(endpoint);
    tab.queryClient = queryClient;
    const requestController = new AbortController();
    const cancelForward = forwardAbort(tab.lifecycle.signal, requestController);
    tab.activeRequest = requestController;

    // null → use the schema's defaultVisible columns; an explicit list (incl.
    // []) ships as-is; a schema-fetch failure attaches nothing.
    let traceMetadataColumns: readonly string[] =
      tab.traceMetadataColumns ?? [];
    if (tab.traceMetadataColumns === null) {
      try {
        const schema = await queryClient.listTraceMetadataSchema(
          settings,
          requestController.signal,
        );
        traceMetadataColumns = effectiveQueryColumns(null, schema.columns);
      } catch {
        traceMetadataColumns = [];
      }
    }
    const executeOptions: ExecuteOptions = {
      traceFilters,
      traceMetadataColumns,
      traceOrderBy,
    };

    const wallStartMs = performance.now();

    try {
      if (tab.materialize) {
        await this.runAsync(
          tab,
          query,
          queryClient,
          settings,
          executeOptions,
          requestController.signal,
          wallStartMs,
        );
      } else {
        await this.runSync(
          tab,
          query,
          queryClient,
          settings,
          executeOptions,
          requestController.signal,
          wallStartMs,
        );
      }
    } catch (e) {
      if (e instanceof QueryCancelledError) {
        tab.isLoading = false;
        this.redraw();
        return;
      }
      tab.queryResult = makeQueryResponse(query, {
        error: e instanceof Error ? e.message : String(e),
        durationMs: performance.now() - wallStartMs,
      });
      tab.isLoading = false;
    } finally {
      cancelForward();
      tab.activeRequest = undefined;
    }

    if (tab.queryResult !== undefined && !tab.materialize) {
      tab.dataSource = new InMemoryDataSource(tab.queryResult.rows);
      tab.isLoading = false;
      this.cb.onHistoryChanged();
    }
    this.redraw();
  }

  // Aborts the local request and, for materialized queries, the remote run too.
  async cancel(tab: BigTraceEditorTab): Promise<void> {
    this.redraw();

    const queryUuid = tab.queryUuid;
    tab.activeRequest?.abort();
    this.poller.stop(tab);

    if (tab.execution && tab.execution.status === 'IN_PROGRESS') {
      tab.execution.status = 'CANCELLED';
      tab.execution.endTime = Date.now();
    }

    if (tab.materialize && queryUuid && tab.queryClient) {
      try {
        await tab.queryClient.cancelQuery(queryUuid);
      } catch (e) {
        console.error(`Failed to cancel query ${queryUuid} on backend:`, e);
      }
    }

    tab.activeRequest = undefined;
    tab.isLoading = false;
    this.cb.onHistoryChanged();
    this.redraw();
  }

  // Pick up a tab whose `queryUuid` was set externally (e.g. history click).
  async resumeFromHistory(
    tab: BigTraceEditorTab,
    fallbackQuery: string,
  ): Promise<void> {
    if (!tab.queryUuid) return;
    const queryClient = new BigtraceQueryClient(getBigtraceEndpoint());
    tab.queryClient = queryClient;

    // The disabled-set reconstruction below needs the global registry (the
    // complement of the snapshot against every categoried setting). Cached, so
    // a no-op once loaded.
    await bigTraceSettingsStorage.loadSettings();

    if (
      !tab.dataSource ||
      (tab.materialize && !(tab.dataSource instanceof BigtraceAsyncDataSource))
    ) {
      tab.dataSource = tab.materialize
        ? new BigtraceAsyncDataSource(
            tab.queryUuid,
            queryClient,
            () => tab.execution?.processedRows ?? 0,
            tab.lifecycle.signal,
          )
        : new InMemoryDataSource([]);
    }

    let details: RawQueryExecution;
    try {
      details = await queryClient.getQueryExecution(
        tab.queryUuid,
        tab.lifecycle.signal,
      );
    } catch (e) {
      if (e instanceof QueryNotFoundError) {
        this.poller.dropStaleQueryUuid(tab);
        return;
      }
      console.error('Failed to fetch query details on open:', e);
      this.poller.start(tab);
      return;
    }

    if (!tab.execution) return;
    const exec = tab.execution;
    exec.status = details.status ?? 'N/A';
    exec.processedRows = details.processedRows ?? 0;
    exec.processedTraces = details.processedTraces ?? 0;
    exec.totalTraces = details.totalTraces ?? 0;
    if (details.limit !== undefined) tab.limit = details.limit;
    // Restore the submit-time snapshot so the settings bar reflects what this
    // query ran with (only the full GET echoes it; the list endpoint omits it).
    // `settings` arrives camelCase (settingId); convert to SettingFilter[].
    const snapshotSettings = snapshotSettingsToFilters(details.settings);
    tab.querySettings = snapshotSettings;
    // Reconstruct the per-tab disabled set: the snapshot lists exactly the
    // settings active for this run, so any categoried global absent from it was
    // disabled at submit time. Skip empty snapshots — there "nothing disabled"
    // and "no snapshot" are indistinguishable, so keep the default (nothing
    // disabled) rather than blanking every toggle.
    if (snapshotSettings.length > 0) {
      tab.disabledSettings = disabledSettingsFromSnapshot(
        snapshotSettings.map((s) => s.settingId),
        bigTraceSettingsStorage
          .buildSettingFilters({includeDisabled: true})
          .map((s) => s.settingId),
      );
    }
    if (Array.isArray(details.traceFilters)) {
      tab.traceFilters = details.traceFilters;
    }
    if (Array.isArray(details.traceMetadataColumns)) {
      tab.traceMetadataColumns = details.traceMetadataColumns;
    }
    if (typeof details.traceOrderBy === 'string') {
      tab.traceOrderBy = details.traceOrderBy;
    }
    this.cb.markDirty?.();
    tab.editorText = details.perfettoSql || fallbackQuery;
    const startMs = isoToEpochMs(details.startTime);
    if (startMs !== undefined) exec.startTime = startMs;

    const isTerminal = TERMINAL_STATUSES.has(exec.status);
    if (isTerminal) {
      const endMs = isoToEpochMs(details.endTime);
      if (endMs !== undefined) exec.endTime = endMs;
    }
    tab.isLoading = !isTerminal;

    const durationMs =
      exec.endTime !== undefined && exec.startTime !== undefined
        ? exec.endTime - exec.startTime
        : 0;

    if (!tab.queryResult) {
      tab.queryResult = makeQueryResponse(tab.editorText, {
        totalRowCount: exec.processedRows,
        durationMs,
        statementWithOutputCount: 1,
      });
    } else {
      if (tab.materialize) {
        tab.queryResult.totalRowCount = exec.processedRows;
      }
      tab.queryResult.lastStatementSql = tab.editorText;
      tab.queryResult.query = tab.editorText;
    }

    if (!isTerminal) {
      this.poller.start(tab);
    } else if (
      (exec.status === 'SUCCESS' || exec.status === 'CANCELLED') &&
      tab.dataSource instanceof BigtraceAsyncDataSource &&
      (details.tableName ?? '') !== '' &&
      exec.processedRows > 0
    ) {
      await tab.dataSource.ensureResultsLoaded();
    } else if (exec.status === 'FAILED') {
      tab.queryResult.error =
        details.errorMessage ??
        'Query failed without a specific error message.';
    }
    this.redraw();
  }

  // For external callers (e.g. resumeFromHistory fallback).
  startPolling(tab: BigTraceEditorTab): void {
    this.poller.start(tab);
  }

  // ----- Internals -----

  private redraw(): void {
    (this.cb.redraw ?? m.redraw)();
  }

  private async runAsync(
    tab: BigTraceEditorTab,
    query: string,
    client: BigtraceQueryClient,
    settings: ReadonlyArray<SettingFilter>,
    options: ExecuteOptions,
    signal: AbortSignal,
    wallStartMs: number,
  ): Promise<void> {
    const data = await client.executeAsync(
      query,
      tab.limit,
      settings,
      signal,
      options,
    );
    if (data.queryUuid === undefined || data.queryUuid === '') {
      throw new Error('Backend did not return a queryUuid for async execute');
    }
    tab.queryUuid = data.queryUuid;
    tab.execution = queryStore.getOrCreate(tab.queryUuid, {
      perfettoSql: query,
    });

    try {
      const details = await client.getQueryExecution(
        tab.queryUuid,
        tab.lifecycle.signal,
      );
      const serverStartMs = isoToEpochMs(details?.startTime);
      if (serverStartMs !== undefined) {
        queryStore.update(tab.queryUuid, {startTime: serverStartMs});
      }
    } catch (e) {
      console.error('Failed to fetch query details after executeAsync:', e);
    }

    this.poller.start(tab);
    tab.dataSource = new BigtraceAsyncDataSource(
      tab.queryUuid,
      client,
      () => tab.execution?.processedRows ?? 0,
      tab.lifecycle.signal,
    );
    tab.queryResult = makeQueryResponse(query, {
      durationMs: performance.now() - wallStartMs,
    });
  }

  private async runSync(
    tab: BigTraceEditorTab,
    query: string,
    client: BigtraceQueryClient,
    settings: ReadonlyArray<SettingFilter>,
    options: ExecuteOptions,
    signal: AbortSignal,
    wallStartMs: number,
  ): Promise<void> {
    const result = await client.executeSync(
      query,
      tab.limit,
      settings,
      signal,
      options,
    );
    if (result.queryUuid === undefined || result.queryUuid === '') {
      throw new Error('Backend did not return a queryUuid for sync execute');
    }
    tab.queryUuid = result.queryUuid;
    tab.execution = queryStore.getOrCreate(tab.queryUuid, {
      perfettoSql: query,
    });
    tab.queryResult = makeQueryResponse(query, {
      rows: [...result.rows],
      columns: [...result.columns],
      totalRowCount: result.rows.length,
      durationMs: performance.now() - wallStartMs,
      statementWithOutputCount: 1,
    });
    queryStore.update(tab.queryUuid, {
      processedRows: result.rows.length,
    });
    tab.isLoading = false;
  }
}
