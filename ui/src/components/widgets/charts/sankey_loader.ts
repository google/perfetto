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
import {
  NUM,
  STR_NULL,
  QueryResult,
} from '../../../trace_processor/query_result';
import {
  ChartSource,
  SQLChartLoader,
  QueryConfig,
  ChartLoaderResult,
  inFilter,
} from './chart_sql_source';
import {ChartAggregation} from './chart_utils';
import {SankeyData, SankeyNode, SankeyLink} from './sankey';

/**
 * Configuration for SQLSankeyLoader.
 */
export interface SQLSankeyLoaderOpts {
  /** The trace processor engine to run queries against. */
  readonly engine: Engine;

  /**
   * SQL query that provides the raw data.
   * Must include source, target, and value columns.
   */
  readonly query: string;

  /** Column name for the source node of each link. */
  readonly sourceColumn: string;

  /** Column name for the target node of each link. */
  readonly targetColumn: string;

  /** Column name for the link value (measure). */
  readonly valueColumn: string;
}

/**
 * Per-use configuration for the sankey loader.
 */
export interface SankeyLoaderConfig {
  /** Aggregation function to apply to the value column. Defaults to 'SUM'. */
  readonly aggregation?: ChartAggregation;

  /** Maximum number of links to return. Defaults to no limit. */
  readonly limit?: number;

  /** Filter to only include specific source nodes. */
  readonly sourceFilter?: ReadonlyArray<string | number>;

  /** Filter to only include specific target nodes. */
  readonly targetFilter?: ReadonlyArray<string | number>;
}

/** Result returned by the sankey loader. */
export type SankeyLoaderResult = ChartLoaderResult<SankeyData>;

/**
 * SQL-based sankey loader with async loading and caching.
 *
 * Builds a flat graph of nodes and weighted links from two dimensions
 * (source, target) and one measure (value). Uses QuerySlot for caching
 * and request deduplication.
 */
export class SQLSankeyLoader extends SQLChartLoader<
  SankeyLoaderConfig,
  SankeyData
> {
  private readonly sourceColumn: string;
  private readonly targetColumn: string;
  private readonly valueColumn: string;

  constructor(opts: SQLSankeyLoaderOpts) {
    super(
      opts.engine,
      new ChartSource({
        query: opts.query,
        schema: {
          [opts.sourceColumn]: 'text',
          [opts.targetColumn]: 'text',
          [opts.valueColumn]: 'real',
        },
      }),
    );
    this.sourceColumn = opts.sourceColumn;
    this.targetColumn = opts.targetColumn;
    this.valueColumn = opts.valueColumn;
  }

  protected buildQueryConfig(config: SankeyLoaderConfig): QueryConfig {
    const aggregation = config.aggregation ?? 'SUM';

    const filters = [
      ...inFilter(this.sourceColumn, config.sourceFilter),
      ...inFilter(this.targetColumn, config.targetFilter),
    ];

    return {
      type: 'aggregated',
      dimensions: [
        {column: this.sourceColumn, alias: '_source'},
        {column: this.targetColumn, alias: '_target'},
      ],
      measures: [{column: this.valueColumn, aggregation}],
      filters,
      limit: config.limit,
      orderDirection: 'desc',
    };
  }

  protected parseResult(queryResult: QueryResult): SankeyData {
    const nodeSet = new Set<string>();
    const links: SankeyLink[] = [];

    const iter = queryResult.iter({
      _source: STR_NULL,
      _target: STR_NULL,
      _value: NUM,
    });

    for (; iter.valid(); iter.next()) {
      const source = iter._source ?? '(null)';
      const target = iter._target ?? '(null)';
      nodeSet.add(source);
      nodeSet.add(target);
      links.push({source, target, value: iter._value});
    }

    const nodes: SankeyNode[] = Array.from(nodeSet, (name) => ({name}));
    return {nodes, links};
  }
}
