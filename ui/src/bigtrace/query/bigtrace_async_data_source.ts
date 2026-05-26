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
import type {QueryResult} from '../../base/query_slot';
import {
  type BigtraceQueryClient,
  QueryCancelledError,
} from './bigtrace_query_client';
import {encodeFilters} from './filter_encoding';
import m from 'mithril';

type ModelWithColumns = DataSourceModel & {
  columns?: Array<{field: string; alias?: string}>;
};

// DataSource adapter paging `:fetch_results` into the DataGrid widget.
export class BigtraceAsyncDataSource implements DataSource {
  private loadedRows: Row[] = [];
  private isFetching = false;
  private columns: string[] = [];
  private error: string | null = null;
  private hasInitialFetchCompleted = false;
  // Window in `loadedRows`, for range-change detection.
  private loadedOffset = 0;
  private loadedLimit = 0;
  // AIP-132 §Ordering. Empty = materialization order.
  private currentOrderBy = '';
  // Aliases pre-resolved to field names. `currentFilterKey` is the JSON
  // form for cheap equality checks.
  private currentFilter: ReadonlyArray<Filter> = [];
  private currentFilterKey = '';
  // `useRows` falls back to `getTotalRows()` when undefined.
  private _filteredTotalRows: number | undefined;

  get filteredTotalRows(): number | undefined {
    return this._filteredTotalRows;
  }

  // `signal`: owner aborts on close. `getTotalRows`: scrollbar sizing.
  constructor(
    private readonly queryUuid: string,
    private readonly queryClient: BigtraceQueryClient,
    private readonly getTotalRows: () => number,
    private readonly signal?: AbortSignal,
  ) {}

  useRows(_model: DataSourceModel): DataSourceRows {
    const model = _model as ModelWithColumns;
    const wantedOrderBy = this.formatOrderBy(model);
    const wantedFilter = this.formatFilter(model);
    const wantedFilterKey = encodeFilters(wantedFilter);
    const wantedOffset = model.pagination?.offset ?? 0;
    const wantedLimit = model.pagination?.limit ?? 0;

    // Fetch on sort/filter/range/initial change; skip if in flight (avoids redraw storms).
    const sortChanged = wantedOrderBy !== this.currentOrderBy;
    const filterChanged = wantedFilterKey !== this.currentFilterKey;
    const rangeChanged =
      this.hasInitialFetchCompleted &&
      (wantedOffset !== this.loadedOffset ||
        (wantedLimit > 0 && wantedLimit !== this.loadedLimit));
    const needsInitial = !this.hasInitialFetchCompleted && wantedLimit > 0;
    if (
      (sortChanged || filterChanged || rangeChanged || needsInitial) &&
      !this.isFetching
    ) {
      this.currentOrderBy = wantedOrderBy;
      if (filterChanged) {
        this.currentFilter = wantedFilter;
        this.currentFilterKey = wantedFilterKey;
        // Briefly oversized scrollbar > briefly collapsed while refetching.
        this._filteredTotalRows = undefined;
      }
      // First render may have limit=0; fall back so the schema comes back.
      const fetchLimit = wantedLimit > 0 ? wantedLimit : 100;
      this.fetchMoreRows(wantedOffset, fetchLimit);
    }

    const mappedRows = this.loadedRows.map((row) => {
      const mappedRow: Row = {};
      for (const key in row) {
        if (Object.prototype.hasOwnProperty.call(row, key)) {
          const col = model.columns?.find((c) => c.field === key);
          const alias =
            col !== undefined && col.alias !== undefined ? col.alias : key;
          mappedRow[alias] = row[key];
        }
      }
      return mappedRow;
    });

    return {
      rows: mappedRows,
      // Filtered total; falls back to unfiltered while undefined.
      totalRows: this._filteredTotalRows ?? this.getTotalRows(),
      rowOffset: this.loadedOffset,
      isPending: this.isFetching,
    };
  }

  // Resolve widget alias → SELECT field (backend whitelists fields).
  private formatOrderBy(model: ModelWithColumns): string {
    const sort = model.sort;
    if (!sort) return '';
    const col = model.columns?.find((c) => c.alias === sort.alias);
    const field = col?.field ?? sort.alias;
    return `${field} ${sort.direction.toLowerCase()}`;
  }

  // Same alias→field remap as formatOrderBy; `fetchResults` does encoding.
  private formatFilter(model: ModelWithColumns): ReadonlyArray<Filter> {
    const filters = model.filters ?? [];
    if (filters.length === 0) return [];
    return filters.map((f) => {
      const col = model.columns?.find((c) => c.alias === f.field);
      const field = col?.field ?? f.field;
      return {...f, field};
    });
  }

  // Re-fetch the currently-loaded window. No-op if a fetch is in flight.
  async refresh(): Promise<void> {
    if (this.isFetching) return;
    const offset = this.loadedOffset;
    const limit = this.loadedLimit > 0 ? this.loadedLimit : 100;
    await this.fetchMoreRows(offset, limit);
  }

  private async fetchMoreRows(offset: number, limit: number) {
    if (this.signal?.aborted) return;
    this.error = null;
    this.isFetching = true;
    m.redraw();
    try {
      const result = await this.queryClient.fetchResults(
        this.queryUuid,
        limit,
        offset,
        this.signal,
        this.currentOrderBy,
        this.currentFilter,
      );
      this.loadedRows = [...result.rows];
      this.loadedOffset = offset;
      this.loadedLimit = limit;
      this.hasInitialFetchCompleted = true;
      this._filteredTotalRows = result.totalFilteredRows;

      if (this.columns.length === 0 && result.columns.length > 0) {
        this.columns = [...result.columns];
      }
    } catch (e) {
      // Abort is expected when the owning tab closes; don't surface it.
      if (e instanceof QueryCancelledError) return;
      console.error('[bigtrace] fetch_results failed:', e);
      this.error = e instanceof Error ? e.message : String(e);
    } finally {
      this.isFetching = false;
      m.redraw();
    }
  }

  // Force the first window after SUCCESS without waiting for a render.
  async ensureResultsLoaded(): Promise<void> {
    if (this.hasInitialFetchCompleted) return;
    await this.fetchMoreRows(0, 100);
  }

  getError(): string | null {
    return this.error;
  }

  getColumns(): string[] {
    return this.columns;
  }

  useAggregateSummaries(_model: DataSourceModel): QueryResult<Row> {
    return {data: undefined, isPending: false, isFresh: true};
  }

  useDistinctValues(
    _column: string | undefined,
  ): QueryResult<readonly SqlValue[]> {
    // `data: []` (not `undefined`) avoids a permanent "Loading…" in the
    // column-filter "Equals" submenu. Cell-context menu filtering still works.
    return {data: [], isPending: false, isFresh: true};
  }

  useParameterKeys(
    _prefix: string | undefined,
  ): QueryResult<readonly string[]> {
    return {data: undefined, isPending: false, isFresh: true};
  }

  async exportData(_model: DataSourceModel): Promise<readonly Row[]> {
    return this.loadedRows;
  }
}
