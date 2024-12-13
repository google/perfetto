// Copyright (C) 2023 The Android Open Source Project
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

import {generateSqlWithInternalLayout} from '../sql_utils/layout';
import {NAMED_ROW, NamedRow, NamedSliceTrack} from './named_slice_track';
import {createView} from '../../trace_processor/sql_utils';
import {Slice} from '../../public/track';
import {AsyncDisposableStack} from '../../base/disposable_stack';
import {sqlNameSafe} from '../../base/string_utils';
import {Trace} from '../../public/trace';
import {Dataset, SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';

export interface CustomSqlTableDefConfig {
  sqlTableName: string;
  columns?: string[];
  whereClause?: string;
}

export abstract class CustomSqlTableSliceTrack extends NamedSliceTrack<
  Slice,
  NamedRow
> {
  protected readonly tableName;

  constructor(trace: Trace, uri: string) {
    super(trace, uri);
    this.tableName = `customsqltableslicetrack_${sqlNameSafe(uri)}`;
  }

  getRowSpec(): NamedRow {
    return NAMED_ROW;
  }

  rowToSlice(row: NamedRow): Slice {
    return this.rowToSliceBase(row);
  }

  // TODO(stevegolton): We should just make this return a dataset going forward,
  // seeing as CustomSqlTableConfig is very similar to a dataset already.
  abstract getSqlDataSource(): CustomSqlTableDefConfig;

  async onInit() {
    const config = this.getSqlDataSource();
    let columns = ['*'];
    if (config.columns !== undefined) {
      columns = config.columns;
    }
    const trash = new AsyncDisposableStack();
    trash.use(
      await createView(
        this.engine,
        this.tableName,
        generateSqlWithInternalLayout({
          columns: columns,
          sourceTable: config.sqlTableName,
          ts: 'ts',
          dur: 'dur',
          whereClause: config.whereClause,
        }),
      ),
    );
    return trash;
  }

  getSqlSource(): string {
    return `SELECT * FROM ${this.tableName}`;
  }

  getDataset(): Dataset {
    return new SourceDataset({
      src: this.makeSqlSelectStatement(),
      schema: {
        id: NUM,
        name: STR,
        ts: LONG,
        dur: LONG,
      },
    });
  }

  private makeSqlSelectStatement(): string {
    const config = this.getSqlDataSource();
    let columns = ['*'];
    if (config.columns !== undefined) {
      columns = config.columns;
    }

    let query = `SELECT ${columns.join(',')} FROM ${config.sqlTableName}`;
    if (config.whereClause) {
      query += ` WHERE ${config.whereClause}`;
    }
    return query;
  }
}
