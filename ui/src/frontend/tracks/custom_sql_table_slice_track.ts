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

import {generateSqlWithInternalLayout} from '../../common/internal_layout_utils';
import {GenericSliceDetailsTabConfigBase} from '../generic_slice_details_tab';
import {NAMED_ROW, NamedRow, NamedSliceTrack} from '../named_slice_track';
import {NewTrackArgs} from '../track';
import {createView} from '../../trace_processor/sql_utils';
import {Slice} from '../../public/track';
import {uuidv4} from '../../base/uuid';
import {AsyncDisposableStack} from '../../base/disposable_stack';

export interface CustomSqlImportConfig {
  modules: string[];
}

export interface CustomSqlTableDefConfig {
  // Table name
  sqlTableName: string;
  // Table columns
  columns?: string[];
  whereClause?: string;
  disposable?: AsyncDisposable;
}

export interface CustomSqlDetailsPanelConfig {
  // Type of details panel to create
  kind: string;
  // Config for the details panel
  config: GenericSliceDetailsTabConfigBase;
}

export abstract class CustomSqlTableSliceTrack extends NamedSliceTrack<
  Slice,
  NamedRow
> {
  protected readonly tableName;

  getRowSpec(): NamedRow {
    return NAMED_ROW;
  }

  rowToSlice(row: NamedRow): Slice {
    return this.rowToSliceBase(row);
  }

  constructor(args: NewTrackArgs) {
    super(args);
    this.tableName = `customsqltableslicetrack_${uuidv4()
      .split('-')
      .join('_')}`;
  }

  abstract getSqlDataSource():
    | CustomSqlTableDefConfig
    | Promise<CustomSqlTableDefConfig>;

  getSqlImports(): CustomSqlImportConfig {
    return {
      modules: [] as string[],
    };
  }

  async onInit() {
    await this.loadImports();
    const config = await Promise.resolve(this.getSqlDataSource());
    let columns = ['*'];
    if (config.columns !== undefined) {
      columns = config.columns;
    }
    const trash = new AsyncDisposableStack();
    config.disposable && trash.use(config.disposable);
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

  async loadImports() {
    for (const importModule of this.getSqlImports().modules) {
      await this.engine.query(`INCLUDE PERFETTO MODULE ${importModule};`);
    }
  }
}
