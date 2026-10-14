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
import type {BigTraceEditorTab, TabQuery} from '../pages/query_tabs_state';
import {serverHasFeature} from '../settings/server_capabilities';

// The local server (the one with 'duckdb') is cheap to ask, so it's polled
// faster than remote backends.
const POLL_INTERVAL_MS = 3000;
const LOCAL_POLL_INTERVAL_MS = 500;
const POLL_RETRY_MS = 1000;

export interface PollingCallbacks {
  readonly redraw: () => void;
  readonly onHistoryChanged: () => void;
}

// Owns the poll-until-terminal loop for async (materialized) queries.
export class PollingController {
  constructor(private readonly cb: PollingCallbacks) {}

  // Begin polling `:status` for `uuids` (by default the tab's queryUuid)
  // until all of them are terminal.
  start(tab: BigTraceEditorTab, uuids = tab.queryUuid ? [tab.queryUuid] : []) {
    if (uuids.length === 0) return;

    const generation = ++tab.pollGeneration;

    const poll = async () => {
      if (tab.pollGeneration !== generation) return;
      if (!tab.isLoading) return;

      try {
        const statuses = await Promise.all(
          uuids.map((uuid) =>
            tab.queryClient?.getStatus(uuid, tab.lifecycle.signal),
          ),
        );
        if (tab.pollGeneration !== generation || !tab.isLoading) return;

        uuids.forEach((uuid, i) => this.applyStatus(uuid, statuses[i]));

        const isTerminal = statuses.every(
          (s) => s?.status !== undefined && TERMINAL_STATUSES.has(s.status),
        );
        if (isTerminal) {
          this.finalize(tab, uuids, statuses);
        } else if (tab.pollInterval !== undefined) {
          tab.isLoading = true;
          tab.pollInterval = window.setTimeout(
            poll,
            serverHasFeature('duckdb')
              ? LOCAL_POLL_INTERVAL_MS
              : POLL_INTERVAL_MS,
          );
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

  // Progress only — the poll never pulls result rows. A page fetch costs a
  // full count over the result table under the same lock the worker writes
  // through, so doing it every 3s would fight the query it is reporting on.
  // The status bar's refresh button (lit while processedRows is ahead of
  // lastProcessedRows) is how a partial result gets asked for.
  private applyStatus(uuid: string, status?: RawQueryExecution): void {
    if (status === undefined) return;
    queryStore.update(uuid, {
      processedRows: status.processedRows ?? 0,
      processedTraces: status.processedTraces ?? 0,
      totalTraces: status.totalTraces ?? 0,
      status: status.status ?? 'N/A',
    });
  }

  private finalize(
    tab: BigTraceEditorTab,
    uuids: ReadonlyArray<string>,
    statuses: ReadonlyArray<RawQueryExecution | undefined>,
  ): void {
    tab.pollInterval = undefined;

    if (tab.isLoading) {
      this.cb.onHistoryChanged();
    }
    tab.isLoading = false;

    for (const q of tab.queries) {
      const i = q.queryUuid === undefined ? -1 : uuids.indexOf(q.queryUuid);
      if (i >= 0) this.finalizeQuery(tab, q, uuids[i], statuses[i]?.status);
    }
    tab.lastProcessedRows = tab.execution?.processedRows ?? 0;
  }

  private finalizeQuery(
    tab: BigTraceEditorTab,
    q: TabQuery,
    uuid: string,
    status?: string,
  ): void {
    if (q.execution !== undefined && q.execution.endTime === undefined) {
      queryStore.update(uuid, {endTime: Date.now()});
    }

    const isFailed = status === 'FAILED';
    const isSuccess = status === 'SUCCESS';

    if (isFailed) {
      const startMs = q.execution?.startTime;
      const endMs = q.execution?.endTime;
      q.queryResult = makeQueryResponse(q.sql, {
        error: 'Fetching error details...',
        durationMs:
          startMs !== undefined && endMs !== undefined ? endMs - startMs : 0,
      });
      this.cb.redraw();
    }

    // Fetch full execution details for timing + error message.
    void tab.queryClient
      ?.getQueryExecution(uuid, tab.lifecycle.signal)
      .then((details: RawQueryExecution) => {
        const endMs = isoToEpochMs(details.endTime);
        queryStore.update(uuid, {
          ...(endMs !== undefined ? {endTime: endMs} : {}),
          ...(details.schema !== undefined ? {schema: details.schema} : {}),
          ...(details.tableName !== undefined
            ? {tableName: details.tableName}
            : {}),
        });
        if (isFailed && q.queryResult !== undefined) {
          q.queryResult.error = details.errorMessage || 'Query failed';
        }
        this.cb.redraw();
      })
      .catch((e: unknown) => {
        console.error('Failed to fetch query execution details:', e);
        if (isFailed && q.queryResult !== undefined) {
          q.queryResult.error = `Failed to fetch error details: ${e instanceof Error ? e.message : String(e)}`;
          this.cb.redraw();
        }
      });

    if (isSuccess && q.dataSource instanceof BigtraceAsyncDataSource) {
      q.dataSource.refresh();
    }
  }
}
