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

import {QuerySlot, SerialTaskQueue} from '../../../../base/query_slot';
import {Engine} from '../../../../trace_processor/engine';
import {NUM, Row} from '../../../../trace_processor/query_result';
import {runQueryForQueryTable} from '../../../query_table/queries';
import {Filter} from '../model';
import {SQLSchemaRegistry} from '../sql_schema';
import {sqlValue} from '../sql_utils';
import {NormalizedQueryModel} from './model';
import {buildQuery} from './query_builder';

export class SQLDataSourceFlat {
  private readonly taskQueue = new SerialTaskQueue();
  private readonly rowCountSlot = new QuerySlot<number>(this.taskQueue);
  private readonly rowsSlot = new QuerySlot<{
    readonly rows: readonly Row[];
    readonly rowOffset: number;
  }>(this.taskQueue);

  constructor(
    private readonly engine: Engine,
    private readonly sqlSchema: SQLSchemaRegistry,
    private readonly rootSchemaName: string,
  ) {}

  useData(model: NormalizedQueryModel) {
    const {columns, filters = [], pagination, sort} = model;

    // Load the row count first
    const {data: rowCount} = this.rowCountSlot.use({
      key: {
        // The row count doesn't depend on pagination or sort
        columns,
        filters: makeFiltersKeyFriendly(filters),
      },
      queryFn: () => this.getRowCount(model),
    });

    const {data: rows} = this.rowsSlot.use({
      key: {
        columns: model.columns,
        filters: makeFiltersKeyFriendly(filters),
        pagination,
        sort,
      },
      retainOn: ['pagination', 'sort'],
      queryFn: async () => {
        const rows = await this.getRows(model);
        return {rows, rowOffset: pagination?.offset ?? 0};
      },
    });

    return {
      totalRows: rowCount,
      rowOffset: rows?.rowOffset,
      rows: rows?.rows,
    };
  }

  private async getRowCount({
    columns,
    filters,
  }: NormalizedQueryModel): Promise<number> {
    const query = buildQuery(this.sqlSchema, this.rootSchemaName, {
      columns,
      filters,
    });
    const result = await this.engine.query(
      `SELECT COUNT(*) as count FROM (${query})`,
    );
    return result.firstRow({count: NUM}).count;
  }

  private async getRows(model: NormalizedQueryModel) {
    const query = buildQuery(this.sqlSchema, this.rootSchemaName, model);
    const result = await runQueryForQueryTable(query, this.engine);
    return result.rows;
  }
}

/**
 * Filters can contain values that are not JSON friendly - e.g. Uint8Arrays.
 * This function returns a version of the filters where such values are
 * converted to their SQL string representation, making them suitable for
 * use in a QuerySlot key.
 */
export function makeFiltersKeyFriendly(
  filters: readonly Filter[],
): readonly any[] {
  return filters.map((f) => {
    if ('value' in f) {
      if (Array.isArray(f.value)) {
        const value = f.value.map((v) =>
          v instanceof Uint8Array ? sqlValue(v) : v,
        );
        return {...f, value};
      } else if (f.value instanceof Uint8Array) {
        return {...f, value: sqlValue(f.value)};
      }
    }
    return f;
  });
}
