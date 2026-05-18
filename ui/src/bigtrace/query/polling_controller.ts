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
import {isoToEpochMs, RawQueryExecution} from './query_history_storage';
import {queryStore, TERMINAL_STATUSES} from './query_store';
import {makeQueryResponse} from '../pages/query_tabs_state';
import type {BigTraceEditorTab} from '../pages/query_tabs_state';

const POLL_INTERVAL_MS = 3000;
const POLL_RETRY_MS = 1000;

export interface PollingCallbacks {
  readonly redraw: () => void;
  readonly onHistoryChanged: () => void;
}

// Owns the poll-until-terminal loop for async (materialized) queries.
// Extracted from QueryRunner so dispatch logic stays separate from the
// polling state machine.
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
          this.applyStatus(tab, status);
          await this.maybeAutoFetchProgress(tab);
        }

        const isTerminal =
          status !== undefined &&
          status.status !== undefined &&
          TERMINAL_STATUSES.has(status.status);
        if (isTerminal) {
          await this.finalize(tab, status!);
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

  private applyStatus(tab: BigTraceEditorTab, status: RawQueryExecution): void {
    if (!tab.queryUuid) return;
    queryStore.update(tab.queryUuid, {
      processedRows: status.processedRows ?? 0,
      processedTraces: status.processedTraces ?? 0,
      totalTraces: status.totalTraces ?? 0,
      status: status.status ?? 'N/A',
    });
    this.cb.redraw();
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

  private async finalize(
    tab: BigTraceEditorTab,
    status: RawQueryExecution,
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
          this.cb.redraw();
        }
      })
      .catch((e: unknown) => {
        console.error('Failed to fetch query execution details:', e);
        if (isFailed && tab.queryResult !== undefined) {
          tab.queryResult.error = `Failed to fetch error details: ${e instanceof Error ? e.message : String(e)}`;
          this.cb.redraw();
        }
      });

    if (isSuccess && tab.dataSource instanceof BigtraceAsyncDataSource) {
      tab.dataSource.refresh();
      tab.lastProcessedRows = tab.execution?.processedRows ?? 0;
    }
  }
}
