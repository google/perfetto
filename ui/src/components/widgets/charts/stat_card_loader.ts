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

import {QuerySlot, SerialTaskQueue} from '../../../base/query_slot';
import {Engine} from '../../../trace_processor/engine';
import {NUM} from '../../../trace_processor/query_result';
import {ChartLoaderResult} from './chart_sql_source';
import {ChartAggregation} from './chart_utils';
import {sqlAggregateExpr} from '../datagrid/sql_utils';

/** Data returned by the stat card loader: a single aggregated value. */
export interface StatCardData {
  readonly value: number;
}

/** Configuration for SQLStatCardLoader. */
export interface SQLStatCardLoaderOpts {
  readonly engine: Engine;
  readonly query: string;
  readonly measureColumn: string;
}

/** Per-use configuration for the stat card loader. */
export interface StatCardLoaderConfig {
  readonly aggregation: ChartAggregation;
}

/** Result returned by the stat card loader. */
export type StatCardLoaderResult = ChartLoaderResult<StatCardData>;

/**
 * SQL-based stat card loader.
 * Computes a single aggregated value (COUNT, SUM, AVG, etc.) from a column.
 */
export class SQLStatCardLoader {
  private readonly engine: Engine;
  private readonly query: string;
  private readonly measureColumn: string;
  private readonly taskQueue = new SerialTaskQueue();
  private readonly querySlot = new QuerySlot<StatCardData>(this.taskQueue);

  constructor(opts: SQLStatCardLoaderOpts) {
    this.engine = opts.engine;
    this.query = opts.query;
    this.measureColumn = opts.measureColumn;
  }

  use(config: StatCardLoaderConfig): StatCardLoaderResult {
    const aggExpr =
      config.aggregation === 'COUNT'
        ? `COUNT(${this.measureColumn})`
        : sqlAggregateExpr(config.aggregation, this.measureColumn);
    const sql = `SELECT ${aggExpr} AS _value FROM (${this.query})`;
    const result = this.querySlot.use({
      key: {sql},
      queryFn: async () => {
        const queryResult = await this.engine.query(sql);
        const iter = queryResult.iter({_value: NUM});
        if (iter.valid()) {
          return {value: iter._value};
        }
        return {value: 0};
      },
    });
    return {data: result.data, isPending: result.isPending};
  }

  dispose(): void {
    this.querySlot.dispose();
  }
}
