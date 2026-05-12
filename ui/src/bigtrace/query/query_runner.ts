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
import {endpointStorage} from '../settings/endpoint_storage';
import {SettingFilter} from '../settings/settings_types';
import {BigtraceAsyncDataSource} from './bigtrace_async_data_source';
import {
  BigtraceQueryClient,
  QueryCancelledError,
  QueryNotFoundError,
} from './bigtrace_query_client';
import {forwardAbort} from './abort_utils';
import {isoToEpochMs, RawQueryExecution} from './query_history_storage';
import {queryStore, TERMINAL_STATUSES} from './query_store';
import {makeQueryResponse} from '../pages/query_tabs_state';
import type {BigTraceEditorTab} from '../pages/query_tabs_state';

const POLL_INTERVAL_MS = 3000;
const POLL_RETRY_MS = 1000;

interface QueryRunnerCallbacks {
  // History panel should refresh (start / finish / cancel).
  readonly onHistoryChanged: () => void;
  // Mithril redraw hook; tests pass a no-op.
  readonly redraw?: () => void;
  // Persist tab list when a tab gains a queryUuid mid-flight.
  readonly markDirty?: () => void;
}

// One instance per QueryPage; owns dispatch / polling / cancel for each tab.
export class QueryRunner {
  constructor(private readonly cb: QueryRunnerCallbacks) {}

  // Run `query` on `tab`. Aborts any in-flight query on the tab first.
  async run(tab: BigTraceEditorTab, query: string): Promise<void> {
    if (!query) return;

    // Abort any in-flight query on this tab.
    tab.activeRequest?.abort();

    tab.isLoading = true;
    tab.queryResult = undefined;
    tab.lastProcessedRows = 0;
    tab.clientStartTime = Date.now();
    this.cb.markDirty?.();
    this.redraw();

    this.cb.onHistoryChanged();
    const endpointSetting = endpointStorage.get('bigtraceEndpoint');
    const endpoint = endpointSetting ? (endpointSetting.get() as string) : '';

    // Empty endpoint resolves to a 404 against the UI server; bail clearly.
    if (endpoint.trim() === '') {
      tab.queryResult = makeQueryResponse(query, {
        error: 'Set the BigTrace Endpoint in Settings before running queries.',
      });
      // renderResultsPanel needs both queryResult and dataSource for the banner.
      tab.dataSource = new InMemoryDataSource([]);
      tab.isLoading = false;
      this.redraw();
      return;
    }

    await bigTraceSettingsStorage.loadSettings();

    const settings = bigTraceSettingsStorage.buildSettingFilters();
    tab.querySettings = settings;

    const queryClient = new BigtraceQueryClient(endpoint);
    tab.queryClient = queryClient;
    // Per-request controller; tab.lifecycle forwards into it on close.
    const requestController = new AbortController();
    const cancelForward = forwardAbort(tab.lifecycle.signal, requestController);
    tab.activeRequest = requestController;
    const wallStartMs = performance.now();

    try {
      if (tab.materialize) {
        await this.runAsync(
          tab,
          query,
          queryClient,
          settings,
          requestController.signal,
          wallStartMs,
        );
      } else {
        await this.runSync(
          tab,
          query,
          queryClient,
          settings,
          requestController.signal,
          wallStartMs,
        );
      }
    } catch (e) {
      // User-initiated cancellation isn't an error worth surfacing.
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
      // Sync skips finalizePolling; flip the sidebar from IN_PROGRESS → SUCCESS here.
      this.cb.onHistoryChanged();
    }
    this.redraw();
  }

  // Aborts the local request and, for materialized queries, the backend too.
  async cancel(tab: BigTraceEditorTab): Promise<void> {
    this.redraw(); // Update UI to show cancelling state.

    const queryUuid = tab.queryUuid;
    tab.activeRequest?.abort();
    if (tab.pollInterval !== undefined) {
      window.clearTimeout(tab.pollInterval);
      tab.pollInterval = undefined;
    }

    // Flip the pill immediately; next poll overwrites with server truth.
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
    const endpointSetting = endpointStorage.get('bigtraceEndpoint');
    const endpoint = endpointSetting ? (endpointSetting.get() as string) : '';
    const queryClient = new BigtraceQueryClient(endpoint);
    tab.queryClient = queryClient;

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
        // Dead UUID (entry deleted / backend restarted); otherwise polls forever.
        this.dropStaleQueryUuid(tab);
        return;
      }
      console.error('Failed to fetch query details on open:', e);
      this.startPolling(tab);
      return;
    }

    if (!tab.execution) return;
    const exec = tab.execution;
    exec.status = details.status ?? 'N/A';
    exec.processedRows = details.processedRows ?? 0;
    exec.processedTraces = details.processedTraces ?? 0;
    exec.totalTraces = details.totalTraces ?? 0;
    if (details.limit !== undefined) tab.limit = details.limit;
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
      // Async only: sync's processedRows is 0 server-side.
      if (tab.materialize) {
        tab.queryResult.totalRowCount = exec.processedRows;
      }
      tab.queryResult.lastStatementSql = tab.editorText;
      tab.queryResult.query = tab.editorText;
    }

    if (!isTerminal) {
      this.startPolling(tab);
    } else if (
      (exec.status === 'SUCCESS' || exec.status === 'CANCELLED') &&
      tab.dataSource instanceof BigtraceAsyncDataSource &&
      // No table → skip the doomed :fetch_results round-trip.
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

  // Poll an already-dispatched async query (tab restore, post-executeAsync).
  startPolling(tab: BigTraceEditorTab): void {
    if (!tab.queryUuid) return;

    // Bump generation so any prior poll self-terminates on next await.
    const generation = ++tab.pollGeneration;

    const poll = async () => {
      if (tab.pollGeneration !== generation) return;
      if (!tab.queryUuid || !tab.isLoading) return;

      try {
        const status = await tab.queryClient?.getStatus(
          tab.queryUuid,
          tab.lifecycle.signal,
        );
        // Re-check: tab may have been cancelled / superseded during the await.
        if (tab.pollGeneration !== generation || !tab.isLoading) return;

        if (status !== undefined && status !== null) {
          this.applyStatus(tab, status);
          await this.maybeAutoFetchProgress(tab);
        }

        const isTerminal =
          status !== undefined &&
          status.status !== undefined &&
          TERMINAL_STATUSES.has(status.status);
        if (isTerminal) {
          await this.finalizePolling(tab, status!);
        } else if (tab.pollInterval !== undefined) {
          tab.isLoading = true;
          tab.pollInterval = window.setTimeout(poll, POLL_INTERVAL_MS);
        }
        this.redraw();
      } catch (e) {
        if (e instanceof QueryNotFoundError) {
          this.dropStaleQueryUuid(tab);
          return;
        }
        console.error('Poll failed:', e);
        if (tab.pollInterval !== undefined) {
          tab.pollInterval = window.setTimeout(poll, POLL_RETRY_MS);
        }
        this.redraw();
      }
    };

    if (tab.pollInterval !== undefined) {
      window.clearTimeout(tab.pollInterval);
    }
    tab.pollInterval = window.setTimeout(poll, 0);
  }

  // Strip dead async metadata but keep the saved SQL for re-run.
  private dropStaleQueryUuid(tab: BigTraceEditorTab): void {
    if (tab.pollInterval !== undefined) {
      window.clearTimeout(tab.pollInterval);
      tab.pollInterval = undefined;
    }
    tab.pollGeneration++;
    tab.queryUuid = undefined;
    tab.execution = undefined;
    tab.dataSource = undefined;
    tab.queryResult = undefined;
    tab.isLoading = false;
    tab.lastProcessedRows = 0;
    this.redraw();
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
    signal: AbortSignal,
    wallStartMs: number,
  ): Promise<void> {
    const data = await client.executeAsync(query, tab.limit, settings, signal);
    if (data.queryUuid === undefined || data.queryUuid === '') {
      throw new Error('Backend did not return a queryUuid for async execute');
    }
    tab.queryUuid = data.queryUuid;
    // Always assign — otherwise the pill stays UNKNOWN after FAILED/SUCCESS.
    tab.execution = queryStore.getOrCreate(tab.queryUuid, tab.execution);

    // Best-effort fetch for a precise server start_time.
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

    this.startPolling(tab);
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
    signal: AbortSignal,
    wallStartMs: number,
  ): Promise<void> {
    const result = await client.executeSync(query, tab.limit, settings, signal);
    if (result.queryUuid === undefined || result.queryUuid === '') {
      throw new Error('Backend did not return a queryUuid for sync execute');
    }
    tab.queryUuid = result.queryUuid;
    tab.execution = queryStore.getOrCreate(tab.queryUuid, tab.execution);
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

  private applyStatus(tab: BigTraceEditorTab, status: RawQueryExecution): void {
    if (!tab.queryUuid) return;
    queryStore.update(tab.queryUuid, {
      processedRows: status.processedRows ?? 0,
      processedTraces: status.processedTraces ?? 0,
      totalTraces: status.totalTraces ?? 0,
      status: status.status ?? 'N/A',
    });
    this.redraw();
  }

  private async maybeAutoFetchProgress(tab: BigTraceEditorTab): Promise<void> {
    const isTerminal =
      tab.execution?.status !== undefined &&
      TERMINAL_STATUSES.has(tab.execution.status);
    if (isTerminal) return;

    const processedRows = tab.execution?.processedRows ?? 0;
    if (processedRows <= tab.lastProcessedRows) return;

    if (tab.dataSource instanceof BigtraceAsyncDataSource) {
      await tab.dataSource.refresh();
      tab.lastProcessedRows = processedRows;
    }
  }

  private async finalizePolling(
    tab: BigTraceEditorTab,
    status: RawQueryExecution,
  ): Promise<void> {
    tab.pollInterval = undefined;

    // Fall back to the local clock if the backend didn't report endTime.
    if (
      tab.execution !== undefined &&
      tab.execution.endTime === undefined &&
      tab.queryUuid
    ) {
      queryStore.update(tab.queryUuid, {endTime: Date.now()});
    }

    // Only refresh history if the query was actively running in the UI.
    if (tab.isLoading) {
      this.cb.onHistoryChanged();
    }
    tab.isLoading = false;

    const isFailed = status.status === 'FAILED';
    const isSuccess = status.status === 'SUCCESS';

    if (isFailed) {
      const startMs = tab.execution?.startTime;
      const endMs = tab.execution?.endTime;
      tab.queryResult = makeQueryResponse(tab.editorText, {
        error: 'Fetching error details...',
        durationMs:
          startMs !== undefined && endMs !== undefined ? endMs - startMs : 0,
      });
      this.redraw();
    }

    // Fetch full execution details for timing + error message.
    void tab.queryClient
      ?.getQueryExecution(tab.queryUuid!, tab.lifecycle.signal)
      .then((details: RawQueryExecution) => {
        const endMs = isoToEpochMs(details.endTime);
        if (endMs !== undefined && tab.queryUuid) {
          queryStore.update(tab.queryUuid, {endTime: endMs});
        }
        if (isFailed && tab.queryResult !== undefined) {
          tab.queryResult.error = details.errorMessage || 'Query failed';
          this.redraw();
        }
      })
      .catch((e: unknown) => {
        console.error('Failed to fetch query execution details:', e);
        if (isFailed && tab.queryResult !== undefined) {
          tab.queryResult.error = `Failed to fetch error details: ${e instanceof Error ? e.message : String(e)}`;
          this.redraw();
        }
      });

    // maybeAutoFetchProgress bails on terminal; without this, restored
    // SUCCESS tabs stay on "Loading schema…".
    if (isSuccess && tab.dataSource instanceof BigtraceAsyncDataSource) {
      tab.dataSource.refresh();
      tab.lastProcessedRows = tab.execution?.processedRows ?? 0;
    }
  }
}
