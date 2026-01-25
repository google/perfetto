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
import {
  QueryResult,
  QuerySlot,
  SerialTaskQueue,
} from '../../../../base/query_slot';
import {shortUuid} from '../../../../base/uuid';
import {Engine} from '../../../../trace_processor/engine';
import {
  Row,
  SqlValue,
  STR,
  UNKNOWN,
} from '../../../../trace_processor/query_result';
import {
  DatagridEngine,
  DataSourceModel,
  DataSourceRows,
} from '../datagrid_engine';
import {
  isSQLExpressionDef,
  SQLSchemaRegistry,
  SQLSchemaResolver,
} from '../sql_schema';
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
  private readonly distinctValuesSlot: QuerySlot<readonly SqlValue[]>;
  private readonly parameterKeysSlot: QuerySlot<readonly string[]>;

  constructor(config: DatagridEngineSQLConfig) {
    this.engine = config.engine;
    this.sqlSchema = config.sqlSchema;
    this.rootSchemaName = config.rootSchemaName;
    this.preamble = config.preamble;
    this.queue = config.queue ?? new SerialTaskQueue();
    this.preambleSlot = new QuerySlot<void>(this.queue);
    this.distinctValuesSlot = new QuerySlot<readonly SqlValue[]>(this.queue);
    this.parameterKeysSlot = new QuerySlot<readonly string[]>(this.queue);
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

  /**
   * Fetch distinct values for a column (for filter dropdowns).
   */
  useDistinctValues(
    column: string | undefined,
  ): QueryResult<readonly SqlValue[]> {
    const {isPending: preamblePending} = this.usePreamble();

    if (column === undefined || preamblePending) {
      return {data: undefined, isPending: preamblePending, isFresh: true};
    }

    return this.distinctValuesSlot.use({
      key: column,
      queryFn: async () => {
        const resolver = new SQLSchemaResolver(
          this.sqlSchema,
          this.rootSchemaName,
        );
        const sqlExpr = resolver.resolveColumnPath(column);
        if (sqlExpr === undefined) {
          return [];
        }

        const baseTable = resolver.getBaseTable();
        const baseAlias = resolver.getBaseAlias();
        const joinClauses = resolver.buildJoinClauses();

        const query = `
          SELECT DISTINCT ${sqlExpr} AS value
          FROM ${baseTable} AS ${baseAlias}
          ${joinClauses}
          ORDER BY 1
          LIMIT 1000
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
  useParameterKeys(prefix: string | undefined): QueryResult<readonly string[]> {
    const {isPending: preamblePending} = this.usePreamble();

    if (prefix === undefined || preamblePending) {
      return {data: undefined, isPending: preamblePending, isFresh: true};
    }

    return this.parameterKeysSlot.use({
      key: prefix,
      queryFn: async () => {
        const rootSchema = this.sqlSchema[this.rootSchemaName];
        if (!rootSchema) {
          return [];
        }

        const colDef = rootSchema.columns[prefix];
        if (
          !colDef ||
          !isSQLExpressionDef(colDef) ||
          !colDef.parameterKeysQuery
        ) {
          return [];
        }

        const baseTable = rootSchema.table;
        const resolver = new SQLSchemaResolver(
          this.sqlSchema,
          this.rootSchemaName,
        );
        const baseAlias = resolver.getBaseAlias();

        const query = colDef.parameterKeysQuery(baseTable, baseAlias);
        const queryResult = await this.engine.query(query);
        const keys: string[] = [];
        for (let it = queryResult.iter({key: UNKNOWN}); it.valid(); it.next()) {
          keys.push(String(it.key));
        }
        return keys;
      },
    });
  }

  async exportData(): Promise<Row[]> {
    throw new Error('Not implemented yet');
  }

  dispose(): void {
    this.preambleSlot.dispose();
    this.distinctValuesSlot.dispose();
    this.parameterKeysSlot.dispose();
    this.flatEngine.dispose();
    this.pivotEngine.dispose();
  }

  /**
   * Run the preamble query if configured. Returns pending status.
   */
  private usePreamble(): QueryResult<void> {
    return this.preambleSlot.use({
      key: this.preamble,
      queryFn: async () => {
        if (this.preamble) {
          await this.engine.query(this.preamble);
        }
      },
    });
  }
}
