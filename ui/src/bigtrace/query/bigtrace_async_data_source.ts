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
import {Row, SqlValue} from '../../trace_processor/query_result';
import {QueryResult} from '../../base/query_slot';
import {HttpDataSource} from './http_data_source';
import m from 'mithril';

type ModelWithColumns = DataSourceModel & {
  columns?: Array<{field: string; alias?: string}>;
};

export class BigtraceAsyncDataSource implements DataSource {
  private queryUuid: string;
  private httpDataSource: HttpDataSource;
  private loadedRows: Row[] = [];
  private isFetching = false;
  private getTotalRows: () => number;
  private getOffset: () => number;
  private columns: string[] = [];
  private error: string | null = null;
  private currentOffset = -1;
  private currentLimit = 0;
  private currentColumnsStr = '';
  private discoveredTotalRows = 0;
  private hasInitialFetchCompleted = false;

  private getPageSize: () => number;

  constructor(
    queryUuid: string,
    httpDataSource: HttpDataSource,
    getTotalRows: () => number,
    getOffset: () => number,
    getPageSize: () => number,
  ) {
    this.queryUuid = queryUuid;
    this.httpDataSource = httpDataSource;
    this.getTotalRows = getTotalRows;
    this.getOffset = getOffset;
    this.getPageSize = getPageSize;

    // Trigger initial fetch to get schema and first batch of data
    this.fetchMoreRows(0, this.getPageSize());
  }

  useRows(_model: DataSourceModel): DataSourceRows {
    const limit = this.getPageSize();
    const offset = this.getOffset();
    const totalRows = Math.max(this.discoveredTotalRows, this.getTotalRows());
    console.log(
      'useRows: offset',
      offset,
      'limit forced to 20',
      'totalRows',
      totalRows,
    );

    const model = _model as ModelWithColumns;
    const columnsStr = JSON.stringify(
      model.columns !== undefined ? model.columns : [],
    );
    const columnsChanged = columnsStr !== this.currentColumnsStr;

    if (columnsChanged) {
      this.currentColumnsStr = columnsStr;
    }

    // Auto-fetch if requested page is different from current cache or columns changed
    if (
      (offset !== this.currentOffset ||
        limit !== this.currentLimit ||
        columnsChanged) &&
      !this.isFetching &&
      offset < totalRows
    ) {
      console.log(
        'useRows: page changed, columns changed or missing, triggering fetch',
      );
      this.fetchMoreRows(offset, limit);
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

    const isPending = this.isFetching;

    return {
      rows: mappedRows,
      totalRows: this.loadedRows.length,
      rowOffset: 0,
      isPending: isPending,
    };
  }

  triggerFetch(offset: number, limit: number) {
    console.log('triggerFetch called for offset', offset, 'limit', limit);
    if (offset === 0) {
      // For first page refresh, we clear the first page rows to force reload
      for (let i = 0; i < limit; i++) {
        delete this.loadedRows[i];
      }
    }
    this.fetchMoreRows(offset, limit);
  }

  private async fetchMoreRows(offset: number, limit: number) {
    console.log('fetchMoreRows: starting for offset', offset, 'limit', limit);
    this.error = null;
    this.isFetching = true;
    m.redraw();
    try {
      const result = await this.httpDataSource.fetchResults(
        this.queryUuid,
        limit,
        offset,
      );
      console.log('fetchMoreRows: received result', JSON.stringify(result));

      this.loadedRows = result.rows;
      this.currentOffset = offset;
      this.currentLimit = limit;
      this.hasInitialFetchCompleted = true;

      // Discover total rows if we received fewer than requested!
      if (result.rows.length < limit) {
        this.discoveredTotalRows = offset + result.rows.length;
        console.log(
          'fetchMoreRows: reached end of results, set discoveredTotalRows to',
          this.discoveredTotalRows,
        );
      }

      console.log(
        'fetchMoreRows: loadedRows length now',
        this.loadedRows.length,
      );
      if (this.columns.length === 0 && result.columns.length > 0) {
        this.columns = result.columns;
        console.log('fetchMoreRows: set columns to', this.columns);
      }
    } catch (e) {
      console.error('Failed to fetch more rows:', e);
      this.error = e instanceof Error ? e.message : String(e);
    } finally {
      this.isFetching = false;
      m.redraw();
    }
  }

  async ensureResultsLoaded(tab: {pageSize: number}): Promise<void> {
    if (this.hasInitialFetchCompleted) {
      return;
    }
    await this.fetchMoreRows(0, tab.pageSize);
  }

  async refresh(tab: {pageSize: number; currentOffset: number}): Promise<void> {
    if (this.isFetching) {
      return;
    }
    await this.fetchMoreRows(tab.currentOffset, tab.pageSize);
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
    return {data: undefined, isPending: false, isFresh: true};
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
