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

import {BigtraceAsyncDataSource} from './bigtrace_async_data_source';
import {QueryNotFoundError} from './bigtrace_query_client';
import {isoToEpochMs, type RawQueryExecution} from './query_history_storage';
import {
  queryStore,
  TERMINAL_STATUSES,
  toQueryExecutionPhase,
} from './query_store';
import {makeQueryResponse} from '../pages/query_tabs_state';
import type {BigTraceEditorTab} from '../pages/query_tabs_state';

const POLL_INTERVAL_MS = 3000;
const POLL_RETRY_MS = 1000;

export interface PollingCallbacks {
  readonly redraw: () => void;
  readonly onHistoryChanged: () => void;
}

// Apply a full GET /query_executions/{uuid} payload to `tab` and `queryStore`.
export function applyExecutionDetails(
  tab: BigTraceEditorTab,
  details: RawQueryExecution,
): void {
  if (!tab.queryUuid) return;
  const status = details.status ?? 'N/A';
  const isTerminal = TERMINAL_STATUSES.has(status);
  const startMs = isoToEpochMs(details.startTime);
  const endMs = isoToEpochMs(details.endTime);

  const exec = queryStore.getOrCreate(tab.queryUuid);
  tab.execution = exec;
  queryStore.update(tab.queryUuid, {
    status,
    processedRows: details.processedRows ?? 0,
    processedTraces: details.processedTraces ?? exec.processedTraces,
    totalTraces: details.totalTraces ?? exec.totalTraces,
    phase: isTerminal
      ? undefined
      : (toQueryExecutionPhase(details.phase) ?? exec.phase),
    ...(details.materialized !== undefined
      ? {materialized: details.materialized}
      : {}),
    ...(startMs !== undefined ? {startTime: startMs} : {}),
    ...(isTerminal && endMs !== undefined ? {endTime: endMs} : {}),
    ...(details.schema !== undefined ? {schema: details.schema} : {}),
    tableName: details.tableName || undefined,
    tableLink: details.tableLink || undefined,
  });

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
    tab.queryResult.totalRowCount = exec.processedRows;
    tab.queryResult.durationMs = durationMs;
    tab.queryResult.lastStatementSql = tab.editorText;
    tab.queryResult.query = tab.editorText;
  }

  if (status === 'FAILED') {
    tab.queryResult.error =
      details.errorMessage || details.error || 'Query failed';
  }
}

// Owns the poll-until-terminal loop for async queries.
export class PollingController {
  private readonly fetchingPreviewDetails = new Set<string>();

  constructor(private readonly cb: PollingCallbacks) {}

  // Begin polling `:status` for the given tab's queryUuid.
  start(tab: BigTraceEditorTab): void {
    if (!tab.queryUuid) return;

    const generation = ++tab.pollGeneration;

    const poll = async () => {
      if (tab.pollGeneration !== generation) return;
      if (!tab.queryUuid || !tab.isLoading) return;

      try {
        const status = await tab.queryClient?.getStatus(
          tab.queryUuid,
          tab.lifecycle.signal,
        );
        if (tab.pollGeneration !== generation || !tab.isLoading) return;

        if (status !== undefined && status !== null) {
          this.applyStatus(tab, status, generation);
        }

        const isTerminal =
          status !== undefined &&
          status.status !== undefined &&
          TERMINAL_STATUSES.has(status.status);
        if (isTerminal) {
          await this.finalize(tab, status!, generation);
        } else if (tab.pollInterval !== undefined) {
          tab.isLoading = true;
          tab.pollInterval = window.setTimeout(poll, POLL_INTERVAL_MS);
        }
        this.cb.redraw();
      } catch (e) {
        if (e instanceof QueryNotFoundError) {
          this.dropStaleQueryUuid(tab);
          return;
        }
        console.error('Poll failed:', e);
        if (tab.pollInterval !== undefined) {
          tab.pollInterval = window.setTimeout(poll, POLL_RETRY_MS);
        }
        this.cb.redraw();
      }
    };

    if (tab.pollInterval !== undefined) {
      window.clearTimeout(tab.pollInterval);
    }
    tab.pollInterval = window.setTimeout(poll, 0);
  }

  // Stop any active poll timer on a tab.
  stop(tab: BigTraceEditorTab): void {
    if (tab.pollInterval !== undefined) {
      window.clearTimeout(tab.pollInterval);
      tab.pollInterval = undefined;
    }
  }

  // Status-bar refresh: re-poll now instead of waiting for the next tick, so a
  // run that finished in between goes through the same `finalize` path, and
  // reload the grid for a run that is still in progress.
  refreshNow(tab: BigTraceEditorTab): void {
    if (!tab.queryUuid) return;
    if (tab.isLoading) {
      if (
        tab.dataSource instanceof BigtraceAsyncDataSource &&
        Boolean(tab.execution?.tableName)
      ) {
        void tab.dataSource.refresh();
      }
      this.start(tab);
      return;
    }
    if (tab.dataSource instanceof BigtraceAsyncDataSource) {
      void tab.dataSource.refresh();
    }
    this.cb.redraw();
  }

  // Strip dead async metadata but keep the saved SQL for re-run.
  dropStaleQueryUuid(tab: BigTraceEditorTab): void {
    this.stop(tab);
    tab.pollGeneration++;
    if (tab.queryUuid) {
      this.fetchingPreviewDetails.delete(tab.queryUuid);
    }
    tab.queryUuid = undefined;
    tab.execution = undefined;
    tab.dataSource = undefined;
    tab.queryResult = undefined;
    tab.isLoading = false;
    this.cb.redraw();
  }

  // ----- Internals -----

  // Progress only — the poll never pulls result rows; mid-run refreshes use
  // the status bar's manual refresh button. `generation` guards every write
  // that happens after an await: a Cancel, re-run or refresh bumps it.
  private applyStatus(
    tab: BigTraceEditorTab,
    status: RawQueryExecution,
    generation: number,
  ): void {
    if (!tab.queryUuid) return;
    const prevRows = tab.execution?.processedRows ?? 0;
    const nextRows = status.processedRows ?? 0;
    const isTerminal =
      status.status !== undefined && TERMINAL_STATUSES.has(status.status);
    const nextPhase = isTerminal
      ? undefined
      : (toQueryExecutionPhase(status.phase) ?? tab.execution?.phase);
    queryStore.update(tab.queryUuid, {
      processedRows: nextRows,
      processedTraces: status.processedTraces ?? 0,
      totalTraces: status.totalTraces ?? 0,
      phase: nextPhase,
      status: status.status ?? 'N/A',
    });
    // `:status` only carries counts; fetch execution details once preview rows
    // appear (and retry if `tableName` is still unset) so DataGrid mounts once
    // with `tableName` and `schema` already populated.
    const uuid = tab.queryUuid;
    if (
      !isTerminal &&
      nextRows > 0 &&
      (prevRows === 0 || !tab.execution?.tableName) &&
      !this.fetchingPreviewDetails.has(uuid)
    ) {
      this.fetchingPreviewDetails.add(uuid);
      void tab.queryClient
        ?.getQueryExecution(uuid, tab.lifecycle.signal)
        .then((details: RawQueryExecution) => {
          if (tab.queryUuid !== uuid || tab.pollGeneration !== generation) {
            return;
          }
          queryStore.update(uuid, {
            ...(details.schema !== undefined ? {schema: details.schema} : {}),
            tableName: details.tableName || undefined,
            tableLink: details.tableLink || undefined,
          });
          this.cb.redraw();
        })
        .catch(() => {})
        .finally(() => {
          this.fetchingPreviewDetails.delete(uuid);
        });
    }
    this.cb.redraw();
  }

  private async finalize(
    tab: BigTraceEditorTab,
    status: RawQueryExecution,
    generation: number,
  ): Promise<void> {
    tab.pollInterval = undefined;

    const isFailed = status.status === 'FAILED';
    const statusError = status.errorMessage || status.error;

    if (tab.queryUuid) {
      queryStore.update(tab.queryUuid, {
        phase: undefined,
        ...(tab.execution !== undefined && tab.execution.endTime === undefined
          ? {endTime: Date.now()}
          : {}),
        ...(isFailed && statusError ? {error: statusError} : {}),
      });
    }

    if (tab.isLoading) {
      this.cb.onHistoryChanged();
    }

    if (isFailed) {
      const startMs = tab.execution?.startTime;
      const endMs = tab.execution?.endTime;
      tab.queryResult = makeQueryResponse(tab.editorText, {
        error: statusError || 'Fetching error details...',
        durationMs:
          startMs !== undefined && endMs !== undefined ? endMs - startMs : 0,
      });
      this.cb.redraw();
    }

    // `isLoading` stays true until the full record (incl. `tableName`) is in,
    // so a redraw in between never shows "Results no longer available".
    const uuid = tab.queryUuid;
    if (uuid && tab.queryClient) {
      try {
        const details = await tab.queryClient.getQueryExecution(
          uuid,
          tab.lifecycle.signal,
        );
        if (tab.queryUuid !== uuid || tab.pollGeneration !== generation) {
          return;
        }
        applyExecutionDetails(tab, details);
      } catch (e: unknown) {
        console.error('Failed to fetch query execution details:', e);
        if (tab.queryUuid !== uuid || tab.pollGeneration !== generation) {
          return;
        }
        if (isFailed && tab.queryResult !== undefined && !statusError) {
          tab.queryResult.error = `Failed to fetch error details: ${e instanceof Error ? e.message : String(e)}`;
        }
      }
    }
    tab.isLoading = false;

    if (
      tab.dataSource instanceof BigtraceAsyncDataSource &&
      Boolean(tab.execution?.tableName) &&
      (tab.execution?.processedRows ?? 0) > 0
    ) {
      await tab.dataSource.refresh();
    }
    this.cb.redraw();
  }
}
