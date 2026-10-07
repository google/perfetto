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

import type {
  DataSource,
  DataSourceModel,
  DataSourceRows,
} from '../../components/widgets/datagrid/data_source';
import type {Filter} from '../../components/widgets/datagrid/model';
import type {Row, SqlValue} from '../../trace_processor/query_result';
import type {AsyncMemoResult} from '../../base/async_memo';
import type {SettingFilter} from '../settings/settings_types';
import {
  type BigtraceQueryClient,
  type ExperimentFilterSpec,
  QueryCancelledError,
} from './bigtrace_query_client';
import {encodeFilters} from './filter_encoding';
import {FetchScheduler} from './fetch_scheduler';
import m from 'mithril';

// Pivot / tree models don't expose `columns`; only the flat model does. Read
// it through a structural type to avoid an `any` cast.
type ModelWithColumns = DataSourceModel & {
  readonly columns?: ReadonlyArray<{readonly field: string}>;
};

// Only TRACE_ADDRESS settings change which traces exist, so the grid refetches
// on those alone — editing another setting leaves the trace set unchanged. The
// full settings array is still sent on each fetch; this only narrows change
// detection.
function experimentFilterKey(filter: ExperimentFilterSpec | undefined): string {
  return filter === undefined
    ? ''
    : `${filter.experimentId}/${filter.controlId}/${filter.isTreatment}`;
}

function traceSourceSettingsKey(
  settings: ReadonlyArray<SettingFilter>,
): string {
  return JSON.stringify(settings.filter((s) => s.category === 'TRACE_ADDRESS'));
}

// DataSource adapter paging `/trace_metadata` into the DataGrid widget — the
// sibling of `BigtraceAsyncDataSource`. Same sort / filter / pagination model,
// but pointed at /trace_metadata instead of a query's results, and re-reading
// the current settings (which carry the trace source) on every fetch.
export class BigtraceTraceListDataSource implements DataSource {
  private loadedRows: Row[] = [];
  // Owns debouncing and cancellation; see FetchScheduler.
  private readonly scheduler: FetchScheduler;
  private columns: string[] = [];
  private error: string | null = null;
  private hasInitialFetchCompleted = false;
  // Window in `loadedRows`, for range-change detection.
  private loadedOffset = 0;
  private loadedLimit = 0;
  // AIP-132 §Ordering. Empty = backend enumeration order.
  private currentOrderBy = '';
  // `currentFilterKey` is the JSON form for cheap equality checks. No alias
  // remap: trace-list columns bind `field === alias`.
  private currentFilter: ReadonlyArray<Filter> = [];
  private currentFilterKey = '';
  private _filteredTotalRows: number | undefined;
  // Settings key at the last fetch. A change (e.g. editing the trace source)
  // invalidates the previous result.
  private lastSettingsKey = '';
  private lastExperimentKey = '';
  // Visible-column projection at the last fetch — both a change trigger and
  // the `columns` field-mask shipped on the next request.
  private currentColumns: readonly string[] = [];
  private currentColumnsKey = '';

  get filteredTotalRows(): number | undefined {
    return this._filteredTotalRows;
  }

  // `getSettings` is a thunk so the Settings page can pass
  // `() => bigTraceSettingsStorage.buildSettingFilters()` and have us re-read
  // it on every render — mirrors `BigtraceAsyncDataSource.getTotalRows`.
  // `onOrderByChange` fires on grid sort change, letting the owner persist the
  // processing order (the snapshot's `trace_order_by`).
  constructor(
    private readonly queryClient: BigtraceQueryClient,
    private readonly getSettings: () => ReadonlyArray<SettingFilter>,
    signal?: AbortSignal,
    private readonly onOrderByChange?: (orderBy: string) => void,
    // Read on every render and every fetch, so the grid shows exactly the
    // corpus a run would see.
    private readonly getExperimentFilter?: () =>
      ExperimentFilterSpec | undefined,
  ) {
    this.scheduler = new FetchScheduler(signal, () => m.redraw());
  }

  useRows(model: DataSourceModel): DataSourceRows {
    const wantedOrderBy = this.formatOrderBy(model);
    const wantedFilter = model.filters ?? [];
    const wantedFilterKey = encodeFilters(wantedFilter);
    const wantedOffset = model.pagination?.offset ?? 0;
    const wantedLimit = model.pagination?.limit ?? 0;
    const wantedSettings = this.getSettings();
    const wantedSettingsKey = traceSourceSettingsKey(wantedSettings);
    // Flat model carries the visible-column field-mask; pivot / tree models
    // don't — those ship no projection, so the server returns defaults.
    const wantedColumns =
      (model as ModelWithColumns).columns?.map((c) => c.field) ?? [];
    const wantedColumnsKey = JSON.stringify(wantedColumns);
    // Keyed on the wire triple alone, so names arriving from the catalog
    // never cost a refetch.
    const wantedExperimentKey = experimentFilterKey(
      this.getExperimentFilter?.(),
    );

    const sortChanged = wantedOrderBy !== this.currentOrderBy;
    const filterChanged = wantedFilterKey !== this.currentFilterKey;
    const rangeChanged =
      this.hasInitialFetchCompleted &&
      (wantedOffset !== this.loadedOffset ||
        (wantedLimit > 0 && wantedLimit !== this.loadedLimit));
    const settingsChanged =
      this.hasInitialFetchCompleted &&
      wantedSettingsKey !== this.lastSettingsKey;
    const columnsChanged =
      this.hasInitialFetchCompleted &&
      wantedColumnsKey !== this.currentColumnsKey;
    const experimentChanged =
      this.hasInitialFetchCompleted &&
      wantedExperimentKey !== this.lastExperimentKey;
    const needsInitial = !this.hasInitialFetchCompleted && wantedLimit > 0;
    // No `isFetching` check: a tick arriving mid-fetch is the newest thing the
    // user has asked for, so it replaces the pending window rather than being
    // dropped. The scheduler coalesces the burst, refuses an identical repeat,
    // and supersedes whatever is still in flight.
    if (
      sortChanged ||
      filterChanged ||
      rangeChanged ||
      settingsChanged ||
      columnsChanged ||
      experimentChanged ||
      needsInitial
    ) {
      this.currentOrderBy = wantedOrderBy;
      // Persist on a real sort change (not the initial fetch, where
      // wantedOrderBy is still '').
      if (sortChanged) {
        this.onOrderByChange?.(wantedOrderBy);
      }
      if (filterChanged) {
        this.currentFilter = wantedFilter;
        this.currentFilterKey = wantedFilterKey;
        // Briefly oversized scrollbar beats briefly collapsed while refetching.
        this._filteredTotalRows = undefined;
      }
      this.currentColumns = wantedColumns;
      this.currentColumnsKey = wantedColumnsKey;
      const fetchLimit = wantedLimit > 0 ? wantedLimit : 100;
      // The first window is what the user is waiting on with nothing on
      // screen, so it goes now; later ones can afford to settle.
      this.scheduler.schedule(
        [
          wantedOffset,
          fetchLimit,
          wantedOrderBy,
          wantedFilterKey,
          wantedColumnsKey,
          wantedSettingsKey,
          wantedExperimentKey,
        ].join('|'),
        needsInitial,
        (controller) =>
          this.fetchWindow(
            wantedOffset,
            fetchLimit,
            wantedSettings,
            controller,
          ),
      );
    }

    return {
      rows: this.loadedRows,
      totalRows: this._filteredTotalRows,
      rowOffset: this.loadedOffset,
      // Pending covers the debounce window too, so the grid doesn't flicker
      // back to "settled" between the last scroll tick and the request.
      isPending: this.scheduler.isPending,
    };
  }

  // Trace-list columns aren't aliased (grid binds `field === alias`), so no
  // alias→field resolution (unlike BigtraceAsyncDataSource).
  private formatOrderBy(model: DataSourceModel): string {
    const sort = model.sort;
    if (!sort) return '';
    return `${sort.alias} ${sort.direction.toLowerCase()}`;
  }

  // Re-fetch the current window with the latest settings. Called by the
  // Settings page when a setting edit doesn't change the grid model, so
  // useRows change-detection wouldn't catch it.
  async refresh(): Promise<void> {
    const offset = this.loadedOffset;
    const limit = this.loadedLimit > 0 ? this.loadedLimit : 100;
    const settings = this.getSettings();
    await this.scheduler.runNow((controller) =>
      this.fetchWindow(offset, limit, settings, controller),
    );
  }

  private async fetchWindow(
    offset: number,
    limit: number,
    settings: ReadonlyArray<SettingFilter>,
    controller: AbortController,
  ): Promise<void> {
    this.error = null;
    this.lastSettingsKey = traceSourceSettingsKey(settings);
    const experimentFilter = this.getExperimentFilter?.();
    this.lastExperimentKey = experimentFilterKey(experimentFilter);
    try {
      const result = await this.queryClient.listTraceMetadata(
        settings,
        limit,
        offset,
        controller.signal,
        this.currentOrderBy,
        this.currentFilter,
        // Empty projection → omit (backend returns its schema defaults).
        this.currentColumns.length > 0 ? this.currentColumns : undefined,
        experimentFilter,
      );
      // A reply that raced its own abort belongs to a window the user has
      // left; adopting it would put back the page they scrolled away from.
      if (!this.scheduler.isCurrent(controller)) return;
      this.loadedRows = [...result.rows];
      this.loadedOffset = offset;
      this.loadedLimit = limit;
      this._filteredTotalRows = result.totalFilteredRows;
      if (result.columns.length > 0) {
        this.columns = [...result.columns];
      }
    } catch (e) {
      if (e instanceof QueryCancelledError) return;
      console.error('[bigtrace] trace_metadata failed:', e);
      this.error = e instanceof Error ? e.message : String(e);
      // A 400 while the trace source is unset/unreadable is the common case
      // mid-edit — drop the rows so the grid doesn't show stale matches.
      this.loadedRows = [];
      this._filteredTotalRows = 0;
    } finally {
      // Flip the flag regardless of success/failure: the first fetch often
      // 400s while the trace source is still empty. Flipping only on success
      // would re-trigger that 400 every render and gate out the
      // settings-changed branch when the user finally sets the source.
      this.hasInitialFetchCompleted = true;
    }
  }

  getError(): string | null {
    return this.error;
  }

  getColumns(): string[] {
    return this.columns;
  }

  useAggregateSummaries(_model: DataSourceModel): AsyncMemoResult<Row> {
    return {data: {}, isPending: false};
  }

  useDistinctValues(
    _column: string | undefined,
  ): AsyncMemoResult<readonly SqlValue[]> {
    // `data: []` (not undefined) keeps the column-filter "Equals" submenu from
    // sticking on "Loading…"; cell-context-menu filtering still works.
    return {data: [], isPending: false};
  }

  useParameterKeys(
    _prefix: string | undefined,
  ): AsyncMemoResult<readonly string[]> {
    return {data: [], isPending: false};
  }

  async exportData(_model: DataSourceModel): Promise<readonly Row[]> {
    return this.loadedRows;
  }
}
