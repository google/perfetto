// Copyright (C) 2025 The Android Open Source Project
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

import {assertUnreachable} from '../../../../base/logging';
import {QueryResult} from '../../../../base/query_slot';
import {exists} from '../../../../base/utils';
import {Engine} from '../../../../trace_processor/engine';
import {Row, SqlValue} from '../../../../trace_processor/query_result';
import {DataSource, DataSourceModel, DataSourceRows} from '../datagrid_engine';
import {SQLSchemaRegistry} from '../sql_schema';
import {FlatEngine} from './datagrid_engine_flat';

export function ensure<T>(x: T): asserts x is NonNullable<T> {
  if (!exists(x)) {
    throw new Error('Value is null or undefined');
  }
}

/**
 * Configuration for SQLDataSource.
 */
export interface SQLDataSourceConfig {
  readonly engine: Engine;
  readonly sqlSchema: SQLSchemaRegistry;
  readonly rootSchemaName: string;
  readonly preamble?: string;
}

/**
 * SQL data source for DataGrid.
 *
 * Simplified version: supports flat mode and pivot mode.
 */
export class SQLDataSource implements DataSource {
  private readonly engine: Engine;
  private readonly sqlSchema: SQLSchemaRegistry;
  private readonly rootSchemaName: string;
  private readonly preamble?: string;
  private readonly flatEngine: FlatEngine;

  constructor(config: SQLDataSourceConfig) {
    this.engine = config.engine;
    this.sqlSchema = config.sqlSchema;
    this.rootSchemaName = config.rootSchemaName;
    this.preamble = config.preamble;

    this.flatEngine = new FlatEngine(
      this.engine,
      this.sqlSchema,
      this.rootSchemaName,
    );
  }

  /**
   * Fetch rows for the current model state.
   */
  useRows(model: DataSourceModel): DataSourceRows {
    const mode = model.mode;
    switch (mode) {
      case 'flat':
        return this.flatEngine.get(model);
      case 'pivot':
        return {rowOffset: 0, isPending: false};
      default:
        assertUnreachable(mode);
    }
  }

  /**
   * Fetch aggregate totals (grand totals across all filtered rows).
   */
  useAggregateTotals(
    _model: DataSourceModel,
  ): QueryResult<ReadonlyMap<string, SqlValue>> {
    return {data: undefined, isPending: false, isFresh: true};
  }

  // Stub implementations for interface compliance
  useDistinctValues(): QueryResult<ReadonlyMap<string, readonly SqlValue[]>> {
    return {data: undefined, isPending: false, isFresh: true};
  }

  useParameterKeys(): QueryResult<ReadonlyMap<string, readonly string[]>> {
    return {data: undefined, isPending: false, isFresh: true};
  }

  async exportData(): Promise<Row[]> {
    throw new Error('Not implemented yet');
  }
}
