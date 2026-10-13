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
  toExperimentFilterSpec,
} from './bigtrace_query_client';
import {forwardAbort} from './abort_utils';
import {
  isoToEpochMs,
  snapshotSettingsToFilters,
  type RawQueryExecution,
} from './query_history_storage';
import {queryStore, TERMINAL_STATUSES} from './query_store';
import {
  buildMultiQuerySpecs,
  disabledSettingsFromSnapshot,
  effectiveTabSettings,
  hasQueryText,
  loadSlotIntoTab,
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
      onHistoryChanged: () => {
        cb.onHistoryChanged();
      },
      markDirty: () => {
        cb.markDirty?.();
      },
    });
  }

  // Run `query` on `tab`. Aborts any in-flight query on the tab first.
  async run(tab: BigTraceEditorTab, query: string): Promise<void> {
    const multiSubQueries =
      tab.queryEngine !== 'duckdb' && tab.subQueries.length > 1
        ? buildMultiQuerySpecs(tab.subQueries)
        : undefined;
    const actualQuery =
      multiSubQueries && multiSubQueries.length > 0
        ? multiSubQueries[0].perfettoSql
        : query;
    if (!hasQueryText(actualQuery)) return;

    tab.activeRequest?.abort();
    this.poller.stop(tab);
    tab.pollGeneration++;

    tab.isLoading = true;
    tab.queryUuid = undefined;
    tab.execution = undefined;
    tab.queryResult = undefined;
    tab.dataSource = undefined;
    tab.lastProcessedRows = 0;
    tab.clientStartTime = Date.now();
    this.cb.markDirty?.();
    this.redraw();

    this.cb.onHistoryChanged();
    const endpoint = getBigtraceEndpoint();

    if (endpoint.trim() === '') {
      tab.queryResult = makeQueryResponse(actualQuery, {
        error:
          'Set the BigTrace endpoint from the connection button (top right) ' +
          'before running queries.',
      });
      tab.dataSource = new InMemoryDataSource([]);
      tab.isLoading = false;
      this.redraw();
      return;
    }

    await bigTraceSettingsStorage.loadSettings();

    const settings = effectiveTabSettings(tab);
    const traceFilters = tab.traceFilters;
    const traceOrderBy = tab.traceOrderBy;

    const queryClient = new BigtraceQueryClient(endpoint);
    tab.queryClient = queryClient;
    const requestController = new AbortController();
    const cancelForward = forwardAbort(tab.lifecycle.signal, requestController);
    tab.activeRequest = requestController;

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
      engine: tab.queryEngine,
      traceFilters,
      traceMetadataColumns,
      traceOrderBy,
      traceLimit: tab.traceLimit,
      experimentFilter: toExperimentFilterSpec(tab.experimentFilter),
      tableTtlDays: tab.materialize ? tab.tableTtlDays : undefined,
      tableName:
        multiSubQueries && multiSubQueries.length > 0
          ? multiSubQueries[0].tableName
          : tab.tableName.trim() || tab.title.trim() || undefined,
      queries:
        multiSubQueries && multiSubQueries.length > 1
          ? multiSubQueries
          : undefined,
    };

    const isDuckdb = tab.queryEngine === 'duckdb';
    if (isDuckdb) {
      tab.materialize = true;
      tab.limit = 0;
    }
    const wallStartMs = performance.now();

    try {
      if (tab.materialize) {
        await this.runAsync(
          tab,
          actualQuery,
          queryClient,
          settings,
          executeOptions,
          requestController.signal,
          wallStartMs,
        );
      } else {
        await this.runSync(
          tab,
          actualQuery,
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
      tab.queryResult = makeQueryResponse(actualQuery, {
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
    for (const slot of tab.subQueries) {
      if (slot.execution && slot.execution.status === 'IN_PROGRESS') {
        slot.execution.status = 'CANCELLED';
        slot.execution.endTime = Date.now();
      }
    }

    if (tab.materialize && tab.queryClient) {
      const uuidsToCancel =
        tab.subQueries.length > 1
          ? tab.subQueries
              .map((s) => s.queryUuid)
              .filter((u): u is string => Boolean(u))
          : queryUuid
            ? [queryUuid]
            : [];
      for (const uuid of uuidsToCancel) {
        try {
          await tab.queryClient.cancelQuery(uuid);
        } catch (e) {
          console.error(`Failed to cancel query ${uuid} on backend:`, e);
        }
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
            () => tab.execution?.schema,
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
    if (details.schema !== undefined) {
      exec.schema = details.schema;
    }
    if (details.tableName !== undefined && details.tableName !== '') {
      exec.tableName = details.tableName;
      tab.tableName = details.tableName;
      tab.title = details.tableName;
    }
    tab.queryEngine = details.queryEngine === 'duckdb' ? 'duckdb' : 'traces';
    exec.status = details.status ?? 'N/A';
    exec.processedRows = details.processedRows ?? 0;
    exec.processedTraces = details.processedTraces ?? 0;
    exec.totalTraces = details.totalTraces ?? 0;
    if (details.limit !== undefined) tab.limit = details.limit;
    if (typeof details.traceLimit === 'number' && details.traceLimit >= 0) {
      tab.traceLimit = details.traceLimit;
    }
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
    // Ids restore now; the names for them are resolved by whichever view
    // shows the filter first.
    if (details.experimentFilter !== undefined) {
      tab.experimentFilter = {...details.experimentFilter};
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

    if (
      data.executions &&
      data.executions.length > 1 &&
      options.queries &&
      options.queries.length > 0
    ) {
      for (
        let i = 0;
        i < data.executions.length && i < options.queries.length;
        i++
      ) {
        const item = data.executions[i];
        const spec = options.queries[i] as {
          slotId?: string;
          perfettoSql: string;
          tableName?: string;
        };
        const slot = tab.subQueries.find((s) => s.id === spec.slotId);
        if (!slot) continue;
        slot.queryUuid = item.queryUuid;
        if (item.tableName) slot.tableName = item.tableName;
        slot.execution = queryStore.getOrCreate(item.queryUuid, {
          perfettoSql: slot.sql,
          tableName: item.tableName,
          status: 'IN_PROGRESS',
        });
        slot.dataSource = new BigtraceAsyncDataSource(
          item.queryUuid,
          client,
          () => slot.execution?.processedRows ?? 0,
          tab.lifecycle.signal,
          () => slot.execution?.schema,
        );
        slot.queryResult = makeQueryResponse(slot.sql, {
          durationMs: performance.now() - wallStartMs,
        });
      }
      loadSlotIntoTab(tab, tab.activeSubQueryIndex);
      const uuids = data.executions.map((e) => e.queryUuid).filter(Boolean);
      this.redraw();
      this.poller.start(tab, uuids);
    } else {
      tab.queryUuid = data.queryUuid;
      tab.execution = queryStore.getOrCreate(tab.queryUuid, {
        perfettoSql: query,
        status: 'IN_PROGRESS',
      });
      tab.dataSource = new BigtraceAsyncDataSource(
        tab.queryUuid,
        client,
        () => tab.execution?.processedRows ?? 0,
        tab.lifecycle.signal,
        () => tab.execution?.schema,
      );
      tab.queryResult = makeQueryResponse(query, {
        durationMs: performance.now() - wallStartMs,
      });
      this.redraw();
      this.poller.start(tab, [data.queryUuid]);
    }

    if (tab.queryUuid) {
      try {
        const details = await client.getQueryExecution(
          tab.queryUuid,
          tab.lifecycle.signal,
        );
        const serverStartMs = isoToEpochMs(details?.startTime);
        queryStore.update(tab.queryUuid, {
          ...(serverStartMs !== undefined ? {startTime: serverStartMs} : {}),
          ...(details?.schema !== undefined ? {schema: details.schema} : {}),
          ...(details?.tableName !== undefined
            ? {tableName: details.tableName}
            : {}),
          ...(details?.processedRows !== undefined
            ? {processedRows: details.processedRows}
            : {}),
          ...(details?.processedTraces !== undefined
            ? {processedTraces: details.processedTraces}
            : {}),
          ...(details?.totalTraces !== undefined
            ? {totalTraces: details.totalTraces}
            : {}),
        });
        if (details?.tableName !== undefined && details.tableName !== '') {
          tab.tableName = details.tableName;
          this.cb.markDirty?.();
        }
        this.redraw();
      } catch (e) {
        console.error('Failed to fetch query details after executeAsync:', e);
      }
    }
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

    if (
      result.executions &&
      result.executions.length > 1 &&
      options.queries &&
      options.queries.length > 0
    ) {
      for (
        let i = 0;
        i < result.executions.length && i < options.queries.length;
        i++
      ) {
        const item = result.executions[i];
        const spec = options.queries[i] as {
          slotId?: string;
          perfettoSql: string;
          tableName?: string;
        };
        const slot = tab.subQueries.find((s) => s.id === spec.slotId);
        if (!slot) continue;
        slot.queryUuid = item.queryUuid;
        if (item.tableName) slot.tableName = item.tableName;
        if (item.queryUuid === result.queryUuid) {
          slot.execution = queryStore.getOrCreate(item.queryUuid, {
            perfettoSql: slot.sql,
            tableName: item.tableName,
          });
          slot.queryResult = makeQueryResponse(slot.sql, {
            rows: [...result.rows],
            columns: [...result.columns],
            schema: result.schema,
            totalRowCount: result.rows.length,
            durationMs: performance.now() - wallStartMs,
            statementWithOutputCount: 1,
          });
          slot.dataSource = new InMemoryDataSource(slot.queryResult.rows);
        } else {
          try {
            const page = await client.fetchResults(
              item.queryUuid,
              tab.limit,
              0,
              signal,
            );
            slot.execution = queryStore.getOrCreate(item.queryUuid, {
              perfettoSql: slot.sql,
              tableName: item.tableName,
            });
            slot.queryResult = makeQueryResponse(slot.sql, {
              rows: [...page.rows],
              columns: [...page.columns],
              schema: page.schema,
              totalRowCount: page.rows.length,
              durationMs: performance.now() - wallStartMs,
              statementWithOutputCount: 1,
            });
            slot.dataSource = new InMemoryDataSource(slot.queryResult.rows);
          } catch (err) {
            slot.queryResult = makeQueryResponse(slot.sql, {
              error: err instanceof Error ? err.message : String(err),
            });
            slot.dataSource = new InMemoryDataSource([]);
          }
        }
      }
      loadSlotIntoTab(tab, tab.activeSubQueryIndex);
    } else {
      tab.queryUuid = result.queryUuid;
      const resolvedTable =
        result.tableName || result.executions?.[0]?.tableName || undefined;
      if (resolvedTable) {
        tab.tableName = resolvedTable;
        this.cb.markDirty?.();
      }
      tab.execution = queryStore.getOrCreate(tab.queryUuid, {
        perfettoSql: query,
        tableName: resolvedTable,
      });
      tab.queryResult = makeQueryResponse(query, {
        rows: [...result.rows],
        columns: [...result.columns],
        schema: result.schema,
        totalRowCount: result.rows.length,
        durationMs: performance.now() - wallStartMs,
        statementWithOutputCount: 1,
      });
      if (tab.queryUuid) {
        queryStore.update(tab.queryUuid, {
          processedRows: result.rows.length,
        });
      }
    }
    tab.isLoading = false;
  }
}
