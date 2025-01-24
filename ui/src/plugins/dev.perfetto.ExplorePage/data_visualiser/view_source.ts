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
import {runQuery} from '../../../components/query_table/queries';
import {ChartAttrs} from '../../../components/widgets/charts/chart';
import {Trace} from '../../../public/trace';
import {Row} from '../../../trace_processor/query_result';
import {QueryNode} from '../query_state';
import {buildFilterSqlClause, VisFilter} from './filters';
import {SqlTableState} from '../../../components/widgets/sql/legacy_table/state';
import {FromSimpleColumn} from '../../../components/widgets/sql/legacy_table/column';

export interface VisViewAttrs {
  charts: Set<ChartAttrs>;
  sqlTableState: SqlTableState;
}

export class VisViewSource {
  readonly trace: Trace;
  readonly queryNode: QueryNode;

  private asyncLimiter = new AsyncLimiter();

  private _baseQuery: string; // Holds original data source query only
  private _fullQuery: string = ''; // Holds query with filter clauses

  private _data?: Row[];
  private _visViews?: VisViewAttrs;
  private _filters?: Set<VisFilter>;
  private _columns?: string[];

  constructor(trace: Trace, queryNode: QueryNode, filters?: Set<VisFilter>) {
    this.trace = trace;
    this.queryNode = queryNode;

    if (filters) {
      this.filters = filters;
    }

    this._baseQuery = `${this.queryNode.imports?.map((i) => `INCLUDE PERFETTO MODULE ${i};`).join('\n')}
      SELECT * FROM (${this.queryNode.getSourceSql()})
    `;

    this.loadData();
  }

  set filters(filters: Set<VisFilter>) {
    this._filters = filters;
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
    this._visViews?.charts.add(vis);
  }

  removeChart(vis: ChartAttrs) {
    this._visViews?.charts.delete(vis);
  }

  addFilter(filter: VisFilter) {
    this.filters?.add(filter);
    this.loadData();
  }

  removeFilter(filter: VisFilter) {
    this.filters?.delete(filter);
    this.loadData();
  }

  private async loadData() {
    let query = this._baseQuery;

    if (query === undefined) return;

    if (this._filters !== undefined) {
      query += ' ' + buildFilterSqlClause(Array.from(this._filters.values()));
    }

    if (query === this._fullQuery) return;

    this._fullQuery = query;

    this.asyncLimiter.schedule(async () => {
      const queryRes = await runQuery(this._fullQuery, this.trace.engine);

      this._data = queryRes.rows;
      this._columns = queryRes.columns;

      this.updateViews(this._data, this._columns);
    });
  }

  private updateViews(data?: Row[], columns?: string[]) {
    const queryNodeColumns = this.queryNode.columns;
    const queryNodeSourceSql = this.queryNode.getSourceSql();

    if (
      data === undefined ||
      columns === undefined ||
      queryNodeColumns === undefined ||
      queryNodeSourceSql === undefined
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
          newChartAttr.columns = columns;

          return newChartAttr;
        },
      );
    }

    const newVisViews: VisViewAttrs = {
      charts: new Set<ChartAttrs>(newChartAttrs),
      sqlTableState: new SqlTableState(this.trace, {
        imports: this.queryNode.imports,
        name: queryNodeSourceSql,
        columns: queryNodeColumns.map(
          (col) =>
            new FromSimpleColumn(col.column.asSimpleColumn(queryNodeSourceSql)),
        ),
      }),
    };

    this._visViews = newVisViews;
  }
}
