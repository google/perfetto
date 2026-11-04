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
import {queryStore, TERMINAL_STATUSES} from './query_store';
import {makeQueryResponse} from '../pages/query_tabs_state';
import type {BigTraceEditorTab} from '../pages/query_tabs_state';

const POLL_INTERVAL_MS = 3000;
const POLL_RETRY_MS = 1000;

export interface PollingCallbacks {
  readonly redraw: () => void;
  readonly onHistoryChanged: () => void;
}

// Owns the poll-until-terminal loop for async BigTrace queries, refreshing the
// F1 table data source when the Phase 1 preview table is published and again
// when Phase 2 finishes replacing it with the full table.
export class PollingController {
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

  // Strip dead async metadata but keep the saved SQL for re-run.
  dropStaleQueryUuid(tab: BigTraceEditorTab): void {
    this.stop(tab);
    tab.pollGeneration++;
    tab.queryUuid = undefined;
    tab.execution = undefined;
    tab.dataSource = undefined;
    tab.queryResult = undefined;
    tab.isLoading = false;
    tab.lastProcessedRows = 0;
    this.cb.redraw();
  }

  // ----- Internals -----

  private applyStatus(
    tab: BigTraceEditorTab,
    status: RawQueryExecution,
    generation: number,
  ): void {
    if (!tab.queryUuid) return;
    const newRows = status.processedRows ?? 0;
    queryStore.update(tab.queryUuid, {
      processedRows: newRows,
      processedTraces: status.processedTraces ?? 0,
      totalTraces: status.totalTraces ?? 0,
      status: status.status ?? 'N/A',
      phase: status.phase,
    });
    // When Phase 1 publishes the preview table (`processedRows > 0` while still
    // in progress), refresh the data source once so the preview rows appear.
    // Terminal transitions refresh in `finalize()` after `getQueryExecution`.
    if (
      !TERMINAL_STATUSES.has(status.status ?? '') &&
      newRows > 0 &&
      newRows !== tab.lastProcessedRows &&
      tab.dataSource instanceof BigtraceAsyncDataSource
    ) {
      void tab.dataSource.refresh();
      if (tab.execution?.tableName === undefined) {
        void this.fetchPreviewDetails(tab, generation);
      }
      tab.lastProcessedRows = newRows;
    }
    this.cb.redraw();
  }

  private async fetchPreviewDetails(
    tab: BigTraceEditorTab,
    generation: number,
  ): Promise<void> {
    if (!tab.queryClient || !tab.queryUuid) return;
    const queryUuid = tab.queryUuid;
    try {
      const details = await tab.queryClient.getQueryExecution(
        queryUuid,
        tab.lifecycle.signal,
      );
      if (tab.pollGeneration !== generation || tab.queryUuid !== queryUuid) {
        return;
      }
      const startMs = isoToEpochMs(details.startTime);
      queryStore.update(queryUuid, {
        ...(startMs !== undefined ? {startTime: startMs} : {}),
        ...(details.schema !== undefined ? {schema: details.schema} : {}),
        ...(details.tableName !== undefined
          ? {tableName: details.tableName}
          : {}),
      });
      this.cb.redraw();
    } catch (e: unknown) {
      console.error('Failed to fetch preview query execution details:', e);
    }
  }

  private async finalize(
    tab: BigTraceEditorTab,
    status: RawQueryExecution,
    generation: number,
  ): Promise<void> {
    tab.pollInterval = undefined;

    if (
      tab.execution !== undefined &&
      tab.execution.endTime === undefined &&
      tab.queryUuid
    ) {
      queryStore.update(tab.queryUuid, {endTime: Date.now()});
    }

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
      this.cb.redraw();
    }

    // Fetch full execution details for timing, tableName, schema, and errors.
    if (tab.queryClient && tab.queryUuid) {
      try {
        const details = await tab.queryClient.getQueryExecution(
          tab.queryUuid,
          tab.lifecycle.signal,
        );
        if (tab.pollGeneration !== generation) return;
        const startMs = isoToEpochMs(details.startTime);
        const endMs = isoToEpochMs(details.endTime);
        if (tab.queryUuid) {
          queryStore.update(tab.queryUuid, {
            ...(startMs !== undefined ? {startTime: startMs} : {}),
            ...(endMs !== undefined ? {endTime: endMs} : {}),
            ...(details.schema !== undefined ? {schema: details.schema} : {}),
            ...(details.tableName !== undefined
              ? {tableName: details.tableName}
              : {}),
            phase: details.phase,
          });
        }
        if (isFailed && tab.queryResult !== undefined) {
          tab.queryResult.error = details.errorMessage || 'Query failed';
        }
      } catch (e: unknown) {
        console.error('Failed to fetch query execution details:', e);
        if (isFailed && tab.queryResult !== undefined) {
          tab.queryResult.error = `Failed to fetch error details: ${e instanceof Error ? e.message : String(e)}`;
        }
      }
    }

    const rows = tab.execution?.processedRows ?? status.processedRows ?? 0;
    if (
      isSuccess &&
      rows > 0 &&
      tab.dataSource instanceof BigtraceAsyncDataSource
    ) {
      void tab.dataSource.refresh();
      tab.lastProcessedRows = rows;
    }
    this.cb.redraw();
  }
}
