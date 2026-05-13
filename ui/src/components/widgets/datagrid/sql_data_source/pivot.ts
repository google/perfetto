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

import {QueryResult, SerialTaskQueue} from '../../../../base/query_slot';
import {Engine} from '../../../../trace_processor/engine';
import {Row} from '../../../../trace_processor/query_result';
import {DataSourceRows, PivotModel} from '../data_source';
import {SQLDataSourceGroupBy} from './group_by';
import {SQLDataSourceRollupTree} from './rollup_tree';
import {SQLSchemaRegistry} from '../sql_schema';

// Pivot datasource for DataGrid - delegates to flat or tree implementations.
export class SQLDataSourcePivot {
  private readonly flat: SQLDataSourceGroupBy;
  private readonly tree: SQLDataSourceRollupTree;

  constructor(
    uuid: string,
    queue: SerialTaskQueue,
    engine: Engine,
    sqlSchema: SQLSchemaRegistry,
    rootSchemaName: string,
  ) {
    this.flat = new SQLDataSourceGroupBy(
      queue,
      engine,
      sqlSchema,
      rootSchemaName,
    );
    this.tree = new SQLDataSourceRollupTree(
      uuid,
      queue,
      engine,
      sqlSchema,
      rootSchemaName,
    );
  }

  getRows(model: PivotModel): DataSourceRows {
    if (model.groupDisplay === 'flat') {
      return this.flat.getRows(model);
    } else {
      return this.tree.getRows(model);
    }
  }

  getSummaries(model: PivotModel): QueryResult<Row> {
    if (model.groupDisplay === 'tree') {
      return this.tree.getSummaries(model);
    } else {
      return this.flat.getSummaries(model);
    }
  }

  exportData(model: PivotModel): Promise<readonly Row[]> {
    return this.flat.exportData(model);
  }

  dispose(): void {
    this.flat.dispose();
    this.tree.dispose();
  }
}
