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

import {Engine} from '../../../trace_processor/engine';
import {NUM, QueryResult} from '../../../trace_processor/query_result';
import {
  ChartSource,
  DEFAULT_MEASURE_ALIAS,
  QueryConfig,
  SQLChartLoader,
} from './chart_sql_source';
import {type StatCardData} from './stat_card';
import {ChartAggregation} from './chart_utils';

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

/**
 * SQL-based stat card loader.
 * Computes a single aggregated value (COUNT, SUM, AVG, etc.) from a column.
 */
export class SQLStatCardLoader extends SQLChartLoader<
  StatCardLoaderConfig,
  StatCardData
> {
  private readonly measureColumn: string;

  constructor(opts: SQLStatCardLoaderOpts) {
    super(
      opts.engine,
      new ChartSource({
        query: opts.query,
        schema: {[opts.measureColumn]: 'real'},
      }),
    );
    this.measureColumn = opts.measureColumn;
  }

  protected buildQueryConfig(config: StatCardLoaderConfig): QueryConfig {
    return {
      type: 'aggregated',
      dimensions: [],
      measures: [{column: this.measureColumn, aggregation: config.aggregation}],
    };
  }

  protected parseResult(queryResult: QueryResult): StatCardData {
    const iter = queryResult.iter({[DEFAULT_MEASURE_ALIAS]: NUM});
    if (iter.valid()) {
      return {value: iter[DEFAULT_MEASURE_ALIAS]};
    }
    return {value: 0};
  }
}
