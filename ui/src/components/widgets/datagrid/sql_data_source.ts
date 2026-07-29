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

import {assertUnreachable} from '../../../base/assert';
import {
  type AsyncMemoResult,
  AsyncMemo,
  AtomicTaskQueue,
} from '../../../base/async_memo';
import {maybeUndefined} from '../../../base/utils';
import {shortUuid} from '../../../base/uuid';
import type {Engine} from '../../../trace_processor/engine';
import {
  type Row,
  type SqlValue,
  UNKNOWN,
} from '../../../trace_processor/query_result';
import type {DataSource, DataSourceModel, DataSourceRows} from './data_source';
import {type SQLTableSchema, SQLSchemaResolver} from './sql_schema';

import {SQLDataSourceFlat} from './sql_data_source/flat';
import {SQLDataSourcePivot} from './sql_data_source/pivot';
import {SQLDataSourceTree} from './sql_data_source/tree';

/**
 * Configuration for SQLDataSource.
 */
export interface DatagridEngineSQLConfig extends SQLTableSchema {
  readonly engine: Engine;
  readonly preamble?: string;
  readonly queue?: AtomicTaskQueue;
}

/**
 * SQL data source for DataGrid.
 *
 * Simplified version: supports flat mode and pivot mode.
 */
export class SQLDataSource implements DataSource {
  private readonly engine: Engine;
  private readonly sqlSchema: SQLTableSchema;
  private readonly preamble?: string;
  private readonly flatEngine: SQLDataSourceFlat;
  private readonly pivotEngine: SQLDataSourcePivot;
  private readonly treeEngine: SQLDataSourceTree;
  private readonly queue: AtomicTaskQueue;
  private readonly preambleSlot: AsyncMemo<void>;
  private readonly distinctValuesSlot: AsyncMemo<readonly SqlValue[]>;
  private readonly parameterKeysSlot: AsyncMemo<readonly string[]>;

  constructor(config: DatagridEngineSQLConfig) {
    this.engine = config.engine;
    this.sqlSchema = config;
    this.preamble = config.preamble;
    this.queue = config.queue ?? new AtomicTaskQueue();
    this.preambleSlot = new AsyncMemo<void>(this.queue);
    this.distinctValuesSlot = new AsyncMemo<readonly SqlValue[]>(this.queue);
    this.parameterKeysSlot = new AsyncMemo<readonly string[]>(this.queue);
    const uuid = shortUuid();

    this.flatEngine = new SQLDataSourceFlat(
      this.queue,
      this.engine,
      this.sqlSchema,
    );

    this.pivotEngine = new SQLDataSourcePivot(
      uuid,
      this.queue,
      this.engine,
      this.sqlSchema,
    );

    this.treeEngine = new SQLDataSourceTree(
      this.queue,
      this.engine,
      this.sqlSchema,
    );
  }

  /**
   * Fetch rows for the current model state.
   */
  useRows(model: DataSourceModel): DataSourceRows {
    try {
      const {isPending: preamblePending} = this.usePreamble();

      // Don't trigger any other queries until the preamble has completed
      if (preamblePending) {
        return {isPending: true};
      }

      const mode = model.mode;
      switch (mode) {
        case 'flat':
          return this.flatEngine.getRows(model);
        case 'pivot':
          return this.pivotEngine.getRows(model);
        case 'tree':
          return this.treeEngine.getRows(model);
        default:
          assertUnreachable(mode);
      }
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      return {isPending: false, error: msg};
    }
  }

  /**
   * Fetch aggregate summaries (aggregates across all filtered rows).
   */
  useAggregateSummaries(model: DataSourceModel): AsyncMemoResult<Row> {
    const mode = model.mode;
    switch (mode) {
      case 'flat':
        return this.flatEngine.getSummaries(model);
      case 'pivot':
        return this.pivotEngine.getSummaries(model);
      case 'tree':
        return this.treeEngine.getSummaries(model);
      default:
        assertUnreachable(mode);
    }
  }

  /**
   * Fetch distinct values for a column (for filter dropdowns).
   */
  useDistinctValues(
    column: string | undefined,
  ): AsyncMemoResult<readonly SqlValue[]> {
    const {isPending: preamblePending} = this.usePreamble();

    if (column === undefined) {
      return {data: [], isPending: false};
    }
    if (preamblePending) {
      return {isPending: true};
    }

    return this.distinctValuesSlot.use({
      key: column,
      compute: async () => {
        const resolver = new SQLSchemaResolver(this.sqlSchema);
        const sqlExpr = resolver.resolveColumnPath(column);
        if (sqlExpr === undefined) {
          return [];
        }

        const baseTable = resolver.getBaseTableOrSubquery();
        const baseAlias = resolver.getBaseAlias();
        const joinClauses = resolver.buildJoinClauses();

        const query = `
          SELECT DISTINCT ${sqlExpr} AS value
          FROM (${baseTable}) AS ${baseAlias}
          ${joinClauses}
          ORDER BY 1
        `;

        const result = await this.engine.query(query);
        const values: SqlValue[] = [];
        for (let it = result.iter({value: UNKNOWN}); it.valid(); it.next()) {
          values.push(it.value);
        }
        return values;
      },
    });
  }

  /**
   * Fetch parameter keys for a parameterized column prefix (e.g., 'args' -> ['foo', 'bar']).
   */
  useParameterKeys(
    prefix: string | undefined,
  ): AsyncMemoResult<readonly string[]> {
    const {isPending: preamblePending} = this.usePreamble();

    if (prefix === undefined) {
      return {data: [], isPending: false};
    }
    if (preamblePending) {
      return {isPending: true};
    }

    return this.parameterKeysSlot.use({
      key: prefix,
      compute: async () => {
        const colDef = maybeUndefined(this.sqlSchema.columns?.[prefix]);
        if (
          !colDef ||
          !('expression' in colDef) ||
          !colDef.parameterKeysQuery
        ) {
          return [];
        }

        const baseTableOrSubquery = this.sqlSchema.tableOrSubquery;
        const resolver = new SQLSchemaResolver(this.sqlSchema);
        const baseAlias = resolver.getBaseAlias();

        const query = colDef.parameterKeysQuery(baseTableOrSubquery, baseAlias);
        const queryResult = await this.engine.query(query);
        const keys: string[] = [];
        for (let it = queryResult.iter({key: UNKNOWN}); it.valid(); it.next()) {
          keys.push(String(it.key));
        }
        return keys;
      },
    });
  }

  /**
   * Returns the SQL that `useRows` would execute for this model, without
   * running it. For pivot/tree modes this includes the statement that
   * materializes their backing table, since traversing it standalone
   * wouldn't mean much - see the mode-specific getQuery for details.
   */
  getQuery(model: DataSourceModel): string {
    const mode = model.mode;
    switch (mode) {
      case 'flat':
        return this.flatEngine.getQuery(model);
      case 'pivot':
        return this.pivotEngine.getQuery(model);
      case 'tree':
        return this.treeEngine.getQuery(model);
      default:
        assertUnreachable(mode);
    }
  }

  async exportData(model: DataSourceModel): Promise<readonly Row[]> {
    switch (model.mode) {
      case 'flat':
        return this.flatEngine.exportData(model);
      case 'pivot':
        return this.pivotEngine.exportData(model);
      case 'tree':
        return this.treeEngine.exportData(model);
    }
  }

  dispose(): void {
    this.preambleSlot.dispose();
    this.distinctValuesSlot.dispose();
    this.parameterKeysSlot.dispose();
    this.flatEngine.dispose();
    this.pivotEngine.dispose();
    this.treeEngine.dispose();
  }

  /**
   * Run the preamble query if configured. Returns pending status.
   */
  private usePreamble(): AsyncMemoResult<void> {
    return this.preambleSlot.use({
      key: {preamble: this.preamble ?? ''},
      compute: async () => {
        if (this.preamble) {
          await this.engine.query(this.preamble);
        }
      },
    });
  }
}
