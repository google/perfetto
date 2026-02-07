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

import {Engine} from '../../trace_processor/engine';
import {UNKNOWN} from '../../trace_processor/query_result';
import {DataSource} from '../../widgets/charts/d3/data/source';
import {Filter, ChartSpec, Row} from '../../widgets/charts/d3/data/types';
import {SqlFactory} from './sql_factory';

/**
 * SqlDataSource implements the DataSource interface using a custom SQL query.
 * It executes the provided SQL query with chart-specific aggregations pushed down to SQL.
 */
export class SqlDataSource implements DataSource {
  private factory: SqlFactory;

  constructor(
    private engine: Engine,
    sqlQuery: string,
  ) {
    this.factory = new SqlFactory(sqlQuery);
  }

  async query(filters: Filter[], spec: ChartSpec): Promise<Row[]> {
    // Generate optimized SQL with chart-specific aggregations
    const sql = this.factory.generateSQL(spec, filters);

    const result = await this.engine.query(sql);

    // Convert QueryResult to Row[]
    const rows: Row[] = [];
    const columns = result.columns();

    // Build iterator spec - use UNKNOWN for all columns
    // to handle any type (strings, numbers, bigints, nulls)
    const iterSpec: Record<string, typeof UNKNOWN> = {};
    for (const col of columns) {
      iterSpec[col] = UNKNOWN;
    }

    for (const it = result.iter(iterSpec); it.valid(); it.next()) {
      const row: Row = {};
      for (const col of columns) {
        const value = it.get(col);
        // Try to convert string values to numbers if they look numeric
        if (typeof value === 'string') {
          const numValue = Number(value);
          row[col] = isNaN(numValue) ? value : numValue;
        } else if (typeof value === 'bigint') {
          row[col] = Number(value);
        } else {
          row[col] = value as string | number | boolean | null;
        }
      }
      rows.push(row);
    }

    return rows;
  }
}
