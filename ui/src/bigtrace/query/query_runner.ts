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
  // Called whenever the runner makes a change that should be reflected in
  // the History panel (start, finish, cancel of a query).
  readonly onHistoryChanged: () => void;
  // Called whenever in-place tab state has been mutated. Defaults to
  // m.redraw if omitted; tests pass a no-op.
  readonly redraw?: () => void;
  // Called when a tab gains a queryUuid mid-flight, so callers can persist
  // the tab list. Defaults to no-op; the QueryPage hands in markDirty.
  readonly markDirty?: () => void;
}

// Owns the lifecycle of one query per tab: dispatch (sync vs async),
// polling for async progress, and cancellation. Single instance per
// QueryPage; works on multiple tabs concurrently.
export class QueryRunner {
  constructor(private readonly cb: QueryRunnerCallbacks) {}

  // Run `query` on `tab`. Aborts any in-flight query on the tab first.
  // Caller is responsible for storing the editor's text on the tab; this
  // method only reads it.
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

    // Empty endpoint → relative URL → 404 against the UI server.
    // Surface a clear message instead.
    if (endpoint.trim() === '') {
      tab.queryResult = makeQueryResponse(query, {
        error: 'Set the BigTrace Endpoint in Settings before running queries.',
      });
      // renderResultsPanel needs both queryResult AND dataSource to
      // show the error banner.
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
    // Per-request controller so a Cancel click only aborts this query.
    // Forward tab.lifecycle aborts to it too — closing the tab must also
    // tear down the in-flight execute_*.
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
      // Sync queries don't go through finalizePolling, so we need to
      // fire onHistoryChanged here for the sidebar to refresh from
      // the IN_PROGRESS row to the SUCCESS one. The async path gets
      // this for free via finalizePolling at the end of polling.
      this.cb.onHistoryChanged();
    }
    this.redraw();
  }

  // Cancel an in-flight query on `tab`. Aborts the local request and, for
  // materialized queries, asks the backend to cancel too.
  async cancel(tab: BigTraceEditorTab): Promise<void> {
    this.redraw(); // Update UI to show cancelling state.

    const queryUuid = tab.queryUuid;
    tab.activeRequest?.abort();
    if (tab.pollInterval !== undefined) {
      window.clearTimeout(tab.pollInterval);
      tab.pollInterval = undefined;
    }

    // Reflect the cancellation locally so the status pill flips
    // immediately; the next poll replaces this with the server's
    // truth.
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

  // Pick up a tab whose `queryUuid` was set externally (typically because
  // the user clicked a row in History). Wires up the data source, fetches
  // current execution metadata, and either resumes polling or eagerly
  // loads results depending on the server-side status.
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
        // Backend doesn't know about this UUID — typically because the
        // entry was deleted from history or the backend was restarted
        // while the UI persisted the tab. Drop the dead reference so the
        // tab behaves like a fresh editor with the saved SQL; otherwise
        // we'd poll forever and the user would see Status: UNKNOWN /
        // Cancel button stuck on screen.
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
      // Only sync `totalRowCount` from `exec.processedRows` for
      // materialized (async) tabs — that's the live count from the
      // materialized table. For sync, `processedRows` is 0 by design
      // (the backend doesn't track row counts for sync) and the
      // inline rows on `tab.queryResult` are the source of truth, so
      // overwriting `totalRowCount` would zero the toolbar's
      // "Returned X rows" summary.
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
      // No materialized table → nothing to fetch. This happens when a
      // query terminates with 0 rows (the backend skips creating the
      // table) or was cancelled before any rows were produced.
      // ensureResultsLoaded would still issue :fetch_results and the
      // backend would return empty / 404; skip the round-trip.
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

  // Begin polling status updates for an async query that's already been
  // dispatched (e.g. on tab restore from localStorage, or right after
  // executeAsync).
  startPolling(tab: BigTraceEditorTab): void {
    if (!tab.queryUuid) return;

    // Bump generation so any in-flight poll from a previous call will
    // notice the mismatch and self-terminate after its next await.
    const generation = ++tab.pollGeneration;

    const poll = async () => {
      // Stale-poll guard: a newer startPolling() supersedes this loop.
      if (tab.pollGeneration !== generation) return;
      if (!tab.queryUuid || !tab.isLoading) return;

      try {
        const status = await tab.queryClient?.getStatus(
          tab.queryUuid,
          tab.lifecycle.signal,
        );
        // Re-check after the await — tab may have been cancelled or
        // superseded while the network request was in flight.
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
          // Backend GC'd or never had this entry. Stop polling and clear
          // the stale UUID so the tab returns to a clean editor state.
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

  // Reset a tab whose persisted queryUuid no longer exists on the backend.
  // The user keeps their saved SQL in the editor and can re-run; we just
  // strip the dead async-execution metadata so the status box, Cancel
  // button and polling loop don't keep running.
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
    // Modern backends emit `queryUuid` as a top-level field; older
    // shapes stuffed it into the single result row, which
    // parseQueryResponse normalizes to the same `queryUuid`. Either
    // way we just read it off the parsed page.
    if (data.queryUuid !== undefined && data.queryUuid !== '') {
      tab.queryUuid = data.queryUuid;
      // Always assign tab.execution to the store entry for this UUID so
      // subsequent applyStatus()/queryStore.update() calls mutate the
      // same object the UI reads from. Previously this was guarded by
      // `if (tab.execution)`, which left fresh tabs with execution
      // undefined — the status pill got stuck on UNKNOWN even after
      // the polling loop saw FAILED/SUCCESS.
      tab.execution = queryStore.getOrCreate(tab.queryUuid, tab.execution);

      // Best-effort fetch for a precise server start_time.
      try {
        const details = await client.getQueryExecution(
          tab.queryUuid,
          tab.lifecycle.signal,
        );
        const serverStartMs = isoToEpochMs(details?.startTime);
        if (serverStartMs !== undefined && tab.queryUuid) {
          queryStore.update(tab.queryUuid, {startTime: serverStartMs});
        }
      } catch (e) {
        console.error('Failed to fetch query details after executeAsync:', e);
      }

      this.startPolling(tab);
    }
    if (tab.queryUuid) {
      tab.dataSource = new BigtraceAsyncDataSource(
        tab.queryUuid,
        client,
        () => tab.execution?.processedRows ?? 0,
        tab.lifecycle.signal,
      );
    }
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
    // Capture the server-assigned UUID into the tab so the tab
    // tracks its most recent run. Mirrors `runAsync`: each Run
    // updates `tab.queryUuid` to the new history entry, even when
    // the tab was originally opened from a different history
    // entry. Backends that don't emit `queryUuid` (the mock) leave
    // it undefined; we fall back to `tab.id` as the QueryStore key
    // as before.
    if (result.queryUuid !== undefined && result.queryUuid !== '') {
      tab.queryUuid = result.queryUuid;
      tab.execution = queryStore.getOrCreate(tab.queryUuid, tab.execution);
    }
    tab.queryResult = makeQueryResponse(query, {
      rows: [...result.rows],
      columns: [...result.columns],
      totalRowCount: result.rows.length,
      durationMs: performance.now() - wallStartMs,
      statementWithOutputCount: 1,
    });
    queryStore.update(tab.queryUuid || tab.id, {
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

    // Auto-fetch results on success. This is NOT redundant with
    // `maybeAutoFetchProgress`: that function bails early for
    // already-terminal queries (`if (isTerminal) return`), so when a
    // tab is restored from localStorage with a UUID whose status is
    // already SUCCESS, the per-poll path never fetches. Without this
    // explicit hook the editor stays on "Loading schema…" forever
    // because columns never arrive. For streaming queries this fires
    // a duplicate fetch of the latest window — accept the round-trip
    // rather than the bug.
    if (isSuccess && tab.dataSource instanceof BigtraceAsyncDataSource) {
      tab.dataSource.refresh();
      tab.lastProcessedRows = tab.execution?.processedRows ?? 0;
    }
  }
}
