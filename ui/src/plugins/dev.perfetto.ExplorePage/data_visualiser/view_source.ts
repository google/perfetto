// Copyright (C) 2024 The Android Open Source Project
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

import {AsyncLimiter} from '../../../base/async_limiter';
import {runQueryForQueryTable} from '../../../components/query_table/queries';
import {ChartAttrs} from '../../../components/widgets/charts/chart';
import {Trace} from '../../../public/trace';
import {Row} from '../../../trace_processor/query_result';
import {QueryNode} from '../query_node';
import {SqlTableState} from '../../../components/widgets/sql/table/state';
import {createTableColumnFromPerfettoSql} from '../../dev.perfetto.SqlModules/sql_modules';
import {analyzeNode, Query} from '../query_builder/query_node_explorer';
import {Filters} from '../../../components/widgets/sql/table/filters';
import {buildSqlQuery} from '../../../components/widgets/sql/table/query_builder';

export interface VisViewAttrs {
  charts: Set<ChartAttrs>;
  sqlTableState: SqlTableState;
}

export class VisViewSource {
  readonly trace: Trace;
  readonly queryNode: QueryNode;
  readonly filters: Filters = new Filters();

  private asyncLimiter = new AsyncLimiter();
  private sqlAsyncLimiter = new AsyncLimiter();

  private _baseQuery?: Query; // Holds original data source query only
  private _fullQuery?: string = ''; // Holds query with filter clauses

  private _data?: Row[];
  private _visViews?: VisViewAttrs;
  private _columns?: string[];

  constructor(trace: Trace, queryNode: QueryNode) {
    this.trace = trace;
    this.queryNode = queryNode;
    this.filters.addObserver(() => this.loadData());

    this.loadBaseQuery();
  }

  get visViews() {
    return this._visViews;
  }

  get data() {
    return this._data;
  }

  get columns() {
    return this._columns;
  }

  addChart(vis: ChartAttrs) {
    return this._visViews?.charts.add(vis);
  }

  removeChart(vis: ChartAttrs) {
    return this._visViews?.charts.delete(vis);
  }

  private async loadData() {
    const baseSql = this._baseQuery?.sql;
    if (baseSql === undefined) return;

    const columns = Object.fromEntries(
      this.queryNode.sourceCols.map((col) => [
        col.column.name,
        col.column.name,
      ]),
    );

    const query = buildSqlQuery({
      prefix: `WITH __data AS (${baseSql})`,
      table: '__data',
      columns: columns,
      filters: this.filters.get(),
    });

    if (query === this._fullQuery) return;

    this._fullQuery = query;

    this.asyncLimiter.schedule(async () => {
      if (this._fullQuery === undefined) {
        return;
      }
      const queryRes = await runQueryForQueryTable(
        this._fullQuery,
        this.trace.engine,
      );

      this._data = queryRes.rows;
      this._columns = queryRes.columns;

      this.updateViews(this._data, this._columns);
    });
  }

  private async loadBaseQuery() {
    this.sqlAsyncLimiter.schedule(async () => {
      const sql = await analyzeNode(this.queryNode, this.trace.engine);
      if (sql instanceof Error) {
        throw sql;
      }
      if (sql === undefined) {
        throw new Error('No SQL query found');
      }
      this._baseQuery = sql;
      this.loadData();
    });
  }

  private updateViews(data?: Row[], columns?: string[]) {
    const queryNodeColumns = this.queryNode.sourceCols;

    if (
      data === undefined ||
      columns === undefined ||
      queryNodeColumns === undefined ||
      this._baseQuery === undefined
    ) {
      return;
    }

    let newChartAttrs;
    if (this._visViews !== undefined) {
      newChartAttrs = Array.from(this._visViews.charts.values()).map(
        (chartAttr) => {
          const newChartAttr = {
            ...chartAttr,
          };

          newChartAttr.data = data;

          return newChartAttr;
        },
      );
    }

    let sqlTableState = this.visViews?.sqlTableState;

    if (sqlTableState === undefined) {
      sqlTableState = new SqlTableState(
        this.trace,
        {
          imports: this._baseQuery.modules,
          prefix: `WITH __data AS (${this._baseQuery.sql})`,
          name: '__data',
          columns: queryNodeColumns.map((col) =>
            // TODO: Figure out how to not require table name here.
            createTableColumnFromPerfettoSql(col.column, ''),
          ),
        },
        {
          filters: this.filters,
        },
      );
    }

    const newVisViews: VisViewAttrs = {
      charts: new Set<ChartAttrs>(newChartAttrs),
      sqlTableState,
    };

    this._visViews = newVisViews;

    this.trace.raf.scheduleFullRedraw();
  }
}
