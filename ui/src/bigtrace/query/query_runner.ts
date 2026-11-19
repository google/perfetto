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
import {dateFilterToFilters, extractDateFilter} from './date_filter';
import {todayIso} from '../base/iso_date';
import {applyExecutionDetails, PollingController} from './polling_controller';

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
    this.poller.stop(tab);
    tab.pollGeneration++;
    tab.queryUuid = undefined;
    tab.execution = undefined;
    tab.dataSource = undefined;
    tab.queryResult = undefined;
    tab.resultColumns = null;
    tab.resultsTabKey = undefined;
    tab.isLoading = true;
    tab.clientStartTime = Date.now();
    this.cb.markDirty?.();
    this.redraw();

    this.cb.onHistoryChanged();
    const endpoint = getBigtraceEndpoint();

    if (endpoint.trim() === '') {
      tab.queryResult = makeQueryResponse(query, {
        error:
          'Set the BigTrace endpoint from the connection button (top right) ' +
          'before running queries.',
      });
      tab.isLoading = false;
      this.redraw();
      return;
    }

    await bigTraceSettingsStorage.loadSettings();

    // Global defaults overridden by per-tab values, minus per-tab-disabled
    // settings. Same helper the trace grid uses, so /trace_metadata and
    // /execute_bigtrace_query_async stay in sync.
    const settings = effectiveTabSettings(tab);

    // Per-tab snapshot is the trace-selection source of truth: what the user
    // staged here (seeded from globals at tab creation, then edited). Globals
    // aren't re-read at run time.
    const traceFilters = [
      ...dateFilterToFilters(tab.dateFilter, todayIso()),
      ...tab.traceFilters,
    ];
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
      traceLimit: tab.traceLimit,
      experimentFilter: toExperimentFilterSpec(tab.experimentFilter),
      tableTtlDays: tab.tableTtlDays,
    };

    const wallStartMs = performance.now();

    try {
      await this.runAsync(
        tab,
        query,
        queryClient,
        settings,
        executeOptions,
        requestController.signal,
        wallStartMs,
      );
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

    this.redraw();
  }

  // Aborts the local request and cancels the remote query execution.
  async cancel(tab: BigTraceEditorTab): Promise<void> {
    this.redraw();

    const queryUuid = tab.queryUuid;
    const queryClient = tab.queryClient;
    tab.activeRequest?.abort();
    this.poller.stop(tab);
    tab.pollGeneration++;

    if (tab.execution && !TERMINAL_STATUSES.has(tab.execution.status)) {
      tab.execution.status = 'CANCELLED';
      tab.execution.phase = undefined;
      tab.execution.endTime = Date.now();
    }

    tab.activeRequest = undefined;
    tab.isLoading = false;

    if (queryUuid && queryClient) {
      try {
        await queryClient.cancelQuery(queryUuid);
        const details = await queryClient.getQueryExecution(
          queryUuid,
          tab.lifecycle.signal,
        );
        if (tab.queryUuid === queryUuid) {
          applyExecutionDetails(tab, details);
          if (!TERMINAL_STATUSES.has(tab.execution?.status ?? '')) {
            this.poller.start(tab);
          } else if (
            tab.dataSource instanceof BigtraceAsyncDataSource &&
            Boolean(tab.execution?.tableName) &&
            (tab.execution?.processedRows ?? 0) > 0
          ) {
            await tab.dataSource.refresh();
          }
        }
      } catch (e) {
        console.error(`Failed to cancel query ${queryUuid} on backend:`, e);
      }
    }

    this.cb.onHistoryChanged();
    this.redraw();
  }

  // Rehydrate a tab restored from localStorage (page reload).
  async rehydrateTab(tab: BigTraceEditorTab): Promise<void> {
    await this.loadExecution(tab, {restoreSettings: false});
  }

  // Status-bar refresh button.
  refreshStatus(tab: BigTraceEditorTab): void {
    this.poller.refreshNow(tab);
  }

  // Pick up a tab whose `queryUuid` was set externally (e.g. history click).
  async resumeFromHistory(
    tab: BigTraceEditorTab,
    fallbackQuery: string,
  ): Promise<void> {
    await this.loadExecution(tab, {restoreSettings: true, fallbackQuery});
  }

  // ----- Internals -----

  private async loadExecution(
    tab: BigTraceEditorTab,
    opts: {readonly restoreSettings: boolean; readonly fallbackQuery?: string},
  ): Promise<void> {
    const uuid = tab.queryUuid;
    if (!uuid) return;
    const queryClient = new BigtraceQueryClient(getBigtraceEndpoint());
    tab.queryClient = queryClient;
    tab.execution = queryStore.getOrCreate(uuid);
    if (!(tab.dataSource instanceof BigtraceAsyncDataSource)) {
      tab.dataSource = new BigtraceAsyncDataSource(
        uuid,
        queryClient,
        () => tab.execution?.processedRows ?? 0,
        tab.lifecycle.signal,
        () => tab.execution?.schema,
      );
    }
    tab.queryResult ??= makeQueryResponse(tab.editorText);

    if (opts.restoreSettings) {
      // The disabled-set reconstruction below needs the global registry (the
      // complement of the snapshot against every categoried setting). Cached,
      // so a no-op once loaded.
      await bigTraceSettingsStorage.loadSettings();
      if (tab.queryUuid !== uuid) return;
    }

    let details: RawQueryExecution;
    try {
      details = await queryClient.getQueryExecution(uuid, tab.lifecycle.signal);
    } catch (e) {
      if (tab.queryUuid !== uuid) return;
      if (e instanceof QueryNotFoundError) {
        this.poller.dropStaleQueryUuid(tab);
        return;
      }
      console.error('Failed to fetch query details on open:', e);
      tab.isLoading = true;
      this.poller.start(tab);
      this.redraw();
      return;
    }
    if (tab.queryUuid !== uuid) return;

    if (opts.restoreSettings) {
      if (details.limit !== undefined) tab.limit = details.limit;
      if (typeof details.traceLimit === 'number' && details.traceLimit >= 0) {
        tab.traceLimit = details.traceLimit;
      }
      // Restore the submit-time snapshot so the settings bar reflects what
      // this query ran with (only the full GET echoes it; the list endpoint
      // omits it). `settings` arrives camelCase (`settingId`); convert to
      // SettingFilter[].
      const snapshotSettings = snapshotSettingsToFilters(details.settings);
      tab.querySettings = snapshotSettings;
      // Reconstruct the per-tab disabled set: the snapshot lists exactly the
      // settings active for this run, so any categoried global absent from it
      // was disabled at submit time. Skip empty snapshots — there "nothing
      // disabled" and "no snapshot" are indistinguishable, so keep the default
      // (nothing disabled) rather than blanking every toggle.
      if (snapshotSettings.length > 0) {
        tab.disabledSettings = disabledSettingsFromSnapshot(
          snapshotSettings.map((s) => s.settingId),
          bigTraceSettingsStorage
            .buildSettingFilters({includeDisabled: true})
            .map((s) => s.settingId),
        );
      }
      if (Array.isArray(details.traceFilters)) {
        // The run's absolute date pair becomes the tab's fixed range, so it is
        // shown on the chip and not doubled up with a default.
        const {rest, dateFilter} = extractDateFilter(details.traceFilters);
        tab.traceFilters = rest;
        if (dateFilter !== undefined) tab.dateFilter = dateFilter;
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
      tab.editorText =
        details.perfettoSql || opts.fallbackQuery || tab.editorText;
      this.cb.markDirty?.();
    }

    applyExecutionDetails(tab, details);
    const exec = tab.execution!;

    if (!TERMINAL_STATUSES.has(exec.status)) {
      this.poller.start(tab);
    }
    if (
      tab.dataSource instanceof BigtraceAsyncDataSource &&
      Boolean(exec.tableName) &&
      exec.processedRows > 0
    ) {
      await tab.dataSource.ensureResultsLoaded();
    }
    this.redraw();
  }

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
    // The backend inserts the row as IN_PROGRESS / PREVIEW; seeding the same
    // values lets polling start at once, with no extra round-trip that a
    // Cancel or re-run could race. The server start time replaces the local
    // one on the first full fetch.
    tab.execution = queryStore.getOrCreate(tab.queryUuid, {
      perfettoSql: query,
      status: 'IN_PROGRESS',
      phase: 'PREVIEW',
      startTime: Date.now(),
    });
    this.cb.onHistoryChanged();

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
    this.poller.start(tab);
  }
}
