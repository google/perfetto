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
import {QueryCancelledError, QueryNotFoundError} from './bigtrace_query_client';
import {isoToEpochMs, type RawQueryExecution} from './query_history_storage';
import {queryStore, TERMINAL_STATUSES} from './query_store';
import type {BigTraceEditorTab} from '../pages/query_tabs_state';

const POLL_INTERVAL_MS = 500;
const POLL_RETRY_MS = 1000;

export interface PollingCallbacks {
  readonly redraw: () => void;
  readonly onHistoryChanged: () => void;
  readonly markDirty?: () => void;
}

function applyDetails(uuid: string, details: RawQueryExecution): void {
  const endMs = isoToEpochMs(details.endTime);
  queryStore.update(uuid, {
    ...(endMs !== undefined ? {endTime: endMs} : {}),
    ...(details.schema !== undefined ? {schema: details.schema} : {}),
    ...(details.tableName !== undefined ? {tableName: details.tableName} : {}),
    ...(details.processedRows !== undefined
      ? {processedRows: details.processedRows}
      : {}),
    ...(details.processedTraces !== undefined
      ? {processedTraces: details.processedTraces}
      : {}),
    ...(details.totalTraces !== undefined
      ? {totalTraces: details.totalTraces}
      : {}),
    ...(details.status !== undefined ? {status: details.status} : {}),
    ...(details.errorMessage !== undefined
      ? {error: details.errorMessage}
      : {}),
  });
}

// Owns the poll-until-terminal loop for async (materialized) queries.
export class PollingController {
  constructor(private readonly cb: PollingCallbacks) {}

  start(tab: BigTraceEditorTab, uuids?: ReadonlyArray<string>): void {
    const pollUuids = (
      uuids && uuids.length > 0 ? uuids : [tab.queryUuid]
    ).filter((u): u is string => Boolean(u));
    if (pollUuids.length === 0) return;

    const generation = ++tab.pollGeneration;

    const poll = async () => {
      if (tab.pollGeneration !== generation || !tab.isLoading) return;

      try {
        const terminalSet = new Set<string>();
        const notFoundSet = new Set<string>();

        await Promise.all(
          pollUuids.map(async (uuid) => {
            try {
              const status = await tab.queryClient?.getStatus(
                uuid,
                tab.lifecycle.signal,
              );
              if (status) {
                queryStore.update(uuid, {
                  processedRows: status.processedRows ?? 0,
                  processedTraces: status.processedTraces ?? 0,
                  totalTraces: status.totalTraces ?? 0,
                  status: status.status ?? 'N/A',
                });
                if (status.status && TERMINAL_STATUSES.has(status.status)) {
                  terminalSet.add(uuid);
                }
              }
            } catch (e) {
              if (e instanceof QueryNotFoundError) {
                terminalSet.add(uuid);
                notFoundSet.add(uuid);
              }
            }
          }),
        );

        if (tab.pollGeneration !== generation || !tab.isLoading) return;

        if (notFoundSet.size === pollUuids.length) {
          this.dropStaleQueryUuid(tab);
          return;
        }

        const activeUuid = tab.queryUuid ?? pollUuids[0];
        const activeExec = queryStore.get(activeUuid);
        if (activeExec !== undefined && tab.queryResult !== undefined) {
          tab.queryResult.totalRowCount = activeExec.processedRows;
        }

        const isTerminal = pollUuids.every((u) => terminalSet.has(u));
        if (isTerminal) {
          await this.finalize(
            tab,
            pollUuids.filter((u) => !notFoundSet.has(u)),
          );
        } else if (tab.pollInterval !== undefined) {
          tab.isLoading = true;
          tab.pollInterval = window.setTimeout(poll, POLL_INTERVAL_MS);
        }
        this.cb.redraw();
      } catch (e) {
        if (
          e instanceof QueryCancelledError ||
          tab.lifecycle.signal.aborted ||
          tab.pollGeneration !== generation
        ) {
          return;
        }
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

  stop(tab: BigTraceEditorTab): void {
    if (tab.pollInterval !== undefined) {
      window.clearTimeout(tab.pollInterval);
      tab.pollInterval = undefined;
    }
  }

  dropStaleQueryUuid(tab: BigTraceEditorTab): void {
    tab.pollInterval = undefined;
    tab.queryUuid = undefined;
    tab.isLoading = false;
    tab.execution = undefined;
    tab.queryResult = undefined;
    tab.dataSource = undefined;
    for (const slot of tab.subQueries) {
      slot.queryUuid = undefined;
      slot.queryResult = undefined;
      slot.dataSource = undefined;
    }
    this.cb.markDirty?.();
    this.cb.redraw();
  }

  private async finalize(
    tab: BigTraceEditorTab,
    pollUuids: ReadonlyArray<string>,
  ): Promise<void> {
    tab.pollInterval = undefined;

    try {
      if (tab.queryClient) {
        await Promise.all(
          pollUuids.map(async (uuid) => {
            queryStore.update(uuid, {endTime: Date.now()});
            try {
              const details = await tab.queryClient?.getQueryExecution(
                uuid,
                tab.lifecycle.signal,
              );
              if (details !== undefined) {
                applyDetails(uuid, details);
              }
            } catch {
              // Ignore details fetch errors
            }
          }),
        );
      }

      for (const slot of tab.subQueries) {
        if (slot.queryUuid === undefined || slot.queryUuid === '') continue;
        const exec = queryStore.get(slot.queryUuid);
        if (exec !== undefined) {
          if (slot.queryResult !== undefined) {
            slot.queryResult.totalRowCount = exec.processedRows;
            if (exec.status === 'FAILED') {
              slot.queryResult.error = exec.error ?? 'Query failed';
            }
          }
          if (exec.tableName !== undefined && exec.tableName !== '') {
            slot.tableName = exec.tableName;
          }
        }
        if (slot.dataSource instanceof BigtraceAsyncDataSource) {
          await slot.dataSource.refresh();
        }
      }

      if (tab.queryUuid !== undefined && tab.queryUuid !== '') {
        const exec = queryStore.get(tab.queryUuid);
        if (exec !== undefined) {
          if (tab.queryResult !== undefined) {
            tab.queryResult.totalRowCount = exec.processedRows;
            if (exec.status === 'FAILED') {
              tab.queryResult.error = exec.error ?? 'Query failed';
            }
          }
          if (exec.tableName !== undefined && exec.tableName !== '') {
            tab.tableName = exec.tableName;
          }
        }
        if (tab.dataSource instanceof BigtraceAsyncDataSource) {
          await tab.dataSource.refresh();
          tab.lastProcessedRows = exec?.processedRows ?? 0;
        }
      }
    } finally {
      if (tab.isLoading) {
        this.cb.onHistoryChanged();
      }
      tab.isLoading = false;
      this.cb.redraw();
    }
  }
}
