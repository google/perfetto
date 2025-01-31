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
import {
  buildFilterSqlClause,
  VisFilter,
  VisFilterOptions,
  VisFilterOp,
  filterToSql,
} from './filters';
import {SqlTableState} from '../../../components/widgets/sql/legacy_table/state';
import {FromSimpleColumn} from '../../../components/widgets/sql/legacy_table/column';
import {SqlColumnAsSimpleColumn} from '../../dev.perfetto.SqlModules/sql_modules';
import {analyzeNode, Query} from '../query_builder/data_source_viewer';
import {
  VegaLiteFieldType,
  VegaLiteSelectionTypes,
} from '../../../components/widgets/vega_view';
import {Item, SignalValue} from 'vega';

export interface VisViewAttrs {
  charts: Set<ChartAttrs>;
  sqlTableState: SqlTableState;
}

export class VisViewSource {
  readonly trace: Trace;
  readonly queryNode: QueryNode;

  private asyncLimiter = new AsyncLimiter();
  private sqlAsyncLimiter = new AsyncLimiter();

  private _baseQuery?: Query; // Holds original data source query only
  private _fullQuery?: string = ''; // Holds query with filter clauses

  private _data?: Row[];
  private _visViews?: VisViewAttrs;
  private _filters: Set<VisFilter> = new Set();
  private _columns?: string[];

  constructor(trace: Trace, queryNode: QueryNode, filters?: Set<VisFilter>) {
    this.trace = trace;
    this.queryNode = queryNode;

    if (filters) {
      this.filters = filters;
    }

    this.loadBaseQuery();
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

  addFilterFromChart(
    selectionType: VegaLiteSelectionTypes,
    fieldName: string,
    fieldType: VegaLiteFieldType,
    filterVal: Item | SignalValue,
  ) {
    let filterOption: VisFilterOp = VisFilterOptions.glob;

    if (selectionType === 'interval') {
      if (fieldType === 'nominal') {
        filterOption = VisFilterOptions.in;
      } else if (fieldType === 'quantitative') {
        filterOption = VisFilterOptions.between;
      }
    } else if (selectionType === 'point') {
      filterOption = VisFilterOptions.equals_to;
    }

    this.addFilter({
      columnName: fieldName,
      value: filterVal,
      filterOption, // Change filter option types to be a record instead
    });
  }

  addFilter(filter: VisFilter) {
    this._filters.add(filter);
    this.loadData();
  }

  removeFilter(filter: VisFilter) {
    this._filters.forEach((visFilter) => {
      if (filterToSql(filter) === filterToSql(visFilter)) {
        this._filters.delete(visFilter);
      }
    });
    this.loadData();
  }

  private async loadData() {
    const query = this._baseQuery;

    if (query === undefined) return;

    if (this._filters.size > 0) {
      query.sql += `WHERE ${buildFilterSqlClause(Array.from(this._filters.values()))}`;
    }

    if (query.sql === this._fullQuery) return;

    this._fullQuery = query.sql;

    this.asyncLimiter.schedule(async () => {
      if (this._fullQuery === undefined) {
        return;
      }
      const queryRes = await runQuery(this._fullQuery, this.trace.engine);

      this._data = queryRes.rows;
      this._columns = queryRes.columns;

      this.updateViews(this._data, this._columns);
    });
  }

  private async loadBaseQuery() {
    this.sqlAsyncLimiter.schedule(async () => {
      const sql = await analyzeNode(this.queryNode, this.trace.engine);
      if (sql === undefined) {
        throw Error(`Couldn't fetch the SQL`);
      }
      this._baseQuery = sql;
      this.loadData();
    });
  }

  private updateViews(data?: Row[], columns?: string[]) {
    const queryNodeColumns = this.queryNode.columns;
    this.loadBaseQuery();

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

    const newVisViews: VisViewAttrs = {
      charts: new Set<ChartAttrs>(newChartAttrs),
      sqlTableState: new SqlTableState(this.trace, {
        imports: this._baseQuery.modules,
        prefix: `WITH vis_view_source_table AS (${this._baseQuery.sql})`,
        name: 'vis_view_source_table',
        columns: queryNodeColumns.map(
          (col) =>
            // TODO: Figure out how to not require table name here.
            new FromSimpleColumn(SqlColumnAsSimpleColumn(col.column, '')),
        ),
      }),
    };

    this._visViews = newVisViews;
  }
}
