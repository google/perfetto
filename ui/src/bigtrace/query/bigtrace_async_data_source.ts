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

import {
  DataSource,
  DataSourceModel,
  DataSourceRows,
} from '../../components/widgets/datagrid/data_source';
import {Filter} from '../../components/widgets/datagrid/model';
import {Row, SqlValue} from '../../trace_processor/query_result';
import {QueryResult} from '../../base/query_slot';
import {
  BigtraceQueryClient,
  QueryCancelledError,
} from './bigtrace_query_client';
import {encodeFilters} from './filter_encoding';
import m from 'mithril';

type ModelWithColumns = DataSourceModel & {
  columns?: Array<{field: string; alias?: string}>;
};

/**
 * `DataSource` adapter that pages a query's materialized result table
 * through `BigtraceQueryClient.fetchResults`, driving the DataGrid
 * widget's virtualized scrolling.
 *
 * Pagination contract
 * -------------------
 * The Grid widget owns the scroll position and reports it on every
 * render via `model.pagination = {offset, limit}`. We hold a single
 * window of rows (`loadedRows`) that covers `[loadedOffset,
 * loadedOffset + loadedLimit)`. When `useRows` is invoked with a
 * `model.pagination` range we don't have, we kick off a fetch for the
 * new range and `loadedRows` is replaced when it returns.
 *
 * `totalRows` comes from `getTotalRows()` (typically the
 * QueryExecution's `processedRows`) so the Grid can size its virtual
 * scrollbar correctly even though only a window is loaded at any time.
 *
 * Sorting contract
 * ----------------
 * The DataGrid widget expresses single-column sort via `model.sort =
 * {alias, direction}` (alias = the column's `id`). We translate that
 * into an [AIP-132 §Ordering](https://google.aip.dev/132#ordering)
 * `order_by` query parameter on the next `:fetch_results` URL.
 *
 * - The widget's `alias` is resolved back to the original SELECT
 *   `field` via the `model.columns` mapping, because the backend's
 *   column whitelist is on field names (not aliases).
 * - On a sort change we refetch the user's *current* viewport
 *   (`model.pagination.offset/limit`). Sort and pagination stay
 *   orthogonal — clicking a header rearranges the data without
 *   jumping the user away from where they were.
 * - Empty / absent sort → no `order_by` is sent; backend returns rows
 *   in materialization order.
 *
 * Filtering contract
 * ------------------
 * The DataGrid widget exposes active filters via `model.filters`, an
 * array of `{field, op, value}` entries built up by the cell-filter
 * and column-filter menus. We JSON-encode it via `encodeFilters`
 * (which canonicalizes object key order and string-coerces every
 * primitive value so the wire is uniformly typed; see
 * `filter_encoding.ts`) and ship it as the `filter` query param on
 * the next `:fetch_results` URL.
 *
 * - Aliases are resolved back to the original SELECT `field` (same
 *   as for sort) because the backend's column whitelist is on field
 *   names.
 * - On a filter change we fetch with the Grid's currently-requested
 *   offset and rely on `totalFilteredRows` from the response to
 *   resize the virtual scrollbar. If the new filter shrinks the set
 *   below the user's offset, the Grid's `onLoadData` clamps and
 *   re-requests a valid range on the next render.
 * - The response's `totalFilteredRows` is what we report for
 *   `useRows`'s `totalRows`. Until the first post-filter fetch
 *   returns we fall back to the unfiltered total from
 *   `getTotalRows()`, so the scrollbar stays sized rather than
 *   collapsing.
 * - Empty filter → no `filter` is sent; the backend computes
 *   `totalFilteredRows` over the entire materialized table.
 */
export class BigtraceAsyncDataSource implements DataSource {
  private loadedRows: Row[] = [];
  private isFetching = false;
  private columns: string[] = [];
  private error: string | null = null;
  private hasInitialFetchCompleted = false;
  // Window currently held in `loadedRows`. Updated whenever a fetch
  // completes successfully so subsequent `useRows` calls can detect
  // whether the Grid wants a different window.
  private loadedOffset = 0;
  private loadedLimit = 0;
  // AIP-132 §Ordering string ("name desc"). Empty = backend returns
  // rows in materialization order.
  private currentOrderBy = '';
  // Active filters with widget aliases resolved to backend field
  // names. Empty = no filter sent on the wire. Kept in structured
  // form so we can hand it straight to `fetchResults`; the matching
  // `currentFilterKey` is the JSON-stringified form for cheap
  // equality comparison against the next render's filters.
  private currentFilter: ReadonlyArray<Filter> = [];
  private currentFilterKey = '';
  // Post-filter row count from the most recent fetch, used to size
  // the DataGrid's virtual scrollbar. Undefined until the first
  // fetch completes (or after a filter change clears it pending the
  // next response); `useRows` falls back to `getTotalRows()` in
  // that interval so the scrollbar doesn't collapse.
  private _filteredTotalRows: number | undefined;

  get filteredTotalRows(): number | undefined {
    return this._filteredTotalRows;
  }

  // The signal is plumbed through to every fetchResults call. Owners
  // (typically a tab) abort it on close so we don't write into a
  // destroyed data source.
  //
  // `getTotalRows` reports the total row count of the materialized
  // table — typically `tab.execution.processedRows`. The Grid uses it
  // to size the virtual scrollbar so the user can navigate the full
  // result set even though only one window is loaded at any time.
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

    // Decide whether to fire a fetch. Four triggers:
    //  1. Sort spec changed.
    //  2. Filter spec changed.
    //  3. The Grid's viewport range no longer overlaps what's loaded.
    //  4. We never fetched at all (initial render).
    // Skip if a fetch is already in flight — the in-flight fetch will
    // settle and trigger another `useRows`, at which point we
    // re-evaluate. This prevents redraw storms.
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
        // Clear the cached count so the scrollbar reverts to the
        // unfiltered total until the response arrives with a fresh
        // `totalFilteredRows`. Briefly oversized > briefly collapsed.
        this._filteredTotalRows = undefined;
      }
      // Use the Grid's requested range. On the very first render the
      // limit may be 0 (model not fully populated yet); fall back to
      // a small default so the schema can come back.
      const fetchLimit = wantedLimit > 0 ? wantedLimit : 100;
      this.fetchMoreRows(wantedOffset, fetchLimit);
    }

    // Map rows to aliases on the fly!
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
      // Prefer the post-filter count when available so the Grid
      // sizes its scrollbar over the visible (filtered) set, not
      // the materialized total. Falls back to the unfiltered total
      // before the first fetch completes or while a filter-change
      // refetch is in flight.
      totalRows: this._filteredTotalRows ?? this.getTotalRows(),
      // Where the loaded rows start in the full result. The Grid uses
      // this to position the rows correctly within its virtualized
      // scroll area.
      rowOffset: this.loadedOffset,
      isPending: this.isFetching,
    };
  }

  // The widget's sort spec is alias-based; the backend's materialized
  // table uses the original SELECT field names. Resolve alias → field
  // before serializing so the column whitelist on the backend matches.
  private formatOrderBy(model: ModelWithColumns): string {
    const sort = model.sort;
    if (!sort) return '';
    const col = model.columns?.find((c) => c.alias === sort.alias);
    const field = col?.field ?? sort.alias;
    return `${field} ${sort.direction.toLowerCase()}`;
  }

  // Same alias→field remap as `formatOrderBy`. The widget's filter
  // chips reference columns by alias; the backend whitelists on the
  // original SELECT field names, so we rewrite each entry's `field`
  // before sending. Returns the structured array (not the JSON
  // string) — `fetchResults` does the encoding itself.
  private formatFilter(model: ModelWithColumns): ReadonlyArray<Filter> {
    const filters = model.filters ?? [];
    if (filters.length === 0) return [];
    return filters.map((f) => {
      const col = model.columns?.find((c) => c.alias === f.field);
      const field = col?.field ?? f.field;
      return {...f, field};
    });
  }

  /**
   * Re-fetch whatever window is currently loaded. Called by the
   * "Refresh" button and by the query runner once the async query
   * reaches a terminal-with-rows state. No-op if a fetch is already
   * in flight.
   */
  async refresh(): Promise<void> {
    if (this.isFetching) return;
    // If nothing has been loaded yet, ask for the first window with a
    // sensible default — virtualization will refine it on the next
    // render.
    const offset = this.loadedOffset;
    const limit = this.loadedLimit > 0 ? this.loadedLimit : 100;
    await this.fetchMoreRows(offset, limit);
  }

  private async fetchMoreRows(offset: number, limit: number) {
    if (this.signal?.aborted) return;
    this.error = null;
    this.isFetching = true;
    // Single funnel for every :fetch_results call we make. Logged at
    // info level so it shows up in DevTools without enabling debug
    // verbosity. `[bigtrace]` prefix is grep-friendly. The filter
    // string is truncated because IN-lists can get long.
    const filterLog =
      this.currentFilterKey.length > 80
        ? this.currentFilterKey.slice(0, 77) + '...'
        : this.currentFilterKey;
    console.log(
      `[bigtrace] fetch_results uuid=${this.queryUuid.slice(0, 8)} ` +
        `offset=${offset} limit=${limit} ` +
        `order_by=${JSON.stringify(this.currentOrderBy)} ` +
        `filter=${filterLog}`,
    );
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

  /**
   * Kicks the data source after the async query reaches SUCCESS so
   * the first window of rows lands without waiting for a render.
   * No-op once any fetch has completed.
   */
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
    // Distinct-values picker is intentionally not implemented for the
    // HTTP-backed source. Returning `data: []` (rather than `undefined`)
    // is what stops the column-filter menu's "Equals" submenu from
    // rendering a permanent "Loading..." — an empty list is honest about
    // there being nothing to pick from. Users can still filter via the
    // cell-context menu (right-click a value → "Add filter > equals"),
    // which goes straight to `model.filters` without touching this path.
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
