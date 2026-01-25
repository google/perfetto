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
import {QueryResult, QuerySlot, SerialTaskQueue} from '../../../../base/query_slot';
import { shortUuid } from '../../../../base/uuid';
import {Engine} from '../../../../trace_processor/engine';
import {Row, SqlValue} from '../../../../trace_processor/query_result';
import {DatagridEngine, DataSourceModel, DataSourceRows} from '../datagrid_engine';
import {SQLSchemaRegistry} from '../sql_schema';
import {FlatEngine} from './flat';
import {PivotEngine} from './pivot';

/**
 * Configuration for SQLDataSource.
 */
export interface DatagridEngineSQLConfig {
  readonly engine: Engine;
  readonly sqlSchema: SQLSchemaRegistry;
  readonly rootSchemaName: string;
  readonly preamble?: string;
  readonly queue?: SerialTaskQueue;
}

/**
 * SQL data source for DataGrid.
 *
 * Simplified version: supports flat mode and pivot mode.
 */
export class DatagridEngineSQL implements DatagridEngine {
  private readonly engine: Engine;
  private readonly sqlSchema: SQLSchemaRegistry;
  private readonly rootSchemaName: string;
  private readonly preamble?: string;
  private readonly flatEngine: FlatEngine;
  private readonly pivotEngine: PivotEngine;
  private readonly queue: SerialTaskQueue;
  private readonly preambleSlot: QuerySlot<void>;

  constructor(config: DatagridEngineSQLConfig) {
    this.engine = config.engine;
    this.sqlSchema = config.sqlSchema;
    this.rootSchemaName = config.rootSchemaName;
    this.preamble = config.preamble;
    this.queue = config.queue ?? new SerialTaskQueue();
    this.preambleSlot = new QuerySlot<void>(this.queue);
    const uuid = shortUuid();

    this.flatEngine = new FlatEngine(
      this.queue,
      this.engine,
      this.sqlSchema,
      this.rootSchemaName,
    );

    this.pivotEngine = new PivotEngine(
      uuid,
      this.queue,
      this.engine,
      this.sqlSchema,
      this.rootSchemaName,
    );
  }

  /**
   * Fetch rows for the current model state.
   */
  useRows(model: DataSourceModel): DataSourceRows {
    const {isPending: preamblePending} = this.preambleSlot.use({
      key: this.preamble,
      queryFn: async () => {
        if (this.preamble) {
          await this.engine.query(this.preamble);
        }
      },
    });

    // Don't trigger any other queries until the preable has completed
    if (preamblePending) {
      return {isPending: true};
    }

    const mode = model.mode;
    switch (mode) {
      case 'flat':
        return this.flatEngine.getRows(model);
      case 'pivot':
        return this.pivotEngine.getRows(model);
      default:
        assertUnreachable(mode);
    }
  }

  /**
   * Fetch aggregate summaries (aggregates across all filtered rows).
   */
  useAggregateSummaries(model: DataSourceModel): QueryResult<Row> {
    const mode = model.mode;
    switch (mode) {
      case 'flat':
        return this.flatEngine.getSummaries(model);
      case 'pivot':
        return this.pivotEngine.getSummaries(model);
      default:
        assertUnreachable(mode);
    }
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

  dispose(): void {
    this.preambleSlot.dispose();
    this.flatEngine.dispose();
    this.pivotEngine.dispose();
  }
}
