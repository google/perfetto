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

import {v4 as uuidv4} from 'uuid';

import {AsyncDisposable, AsyncDisposableStack} from '../../base/disposable';
import {Actions} from '../../common/actions';
import {generateSqlWithInternalLayout} from '../../common/internal_layout_utils';
import {LegacySelection} from '../../common/state';
import {OnSliceClickArgs} from '../base_slice_track';
import {GenericSliceDetailsTabConfigBase} from '../generic_slice_details_tab';
import {globals} from '../globals';
import {NamedSliceTrack, NamedSliceTrackTypes} from '../named_slice_track';
import {NewTrackArgs} from '../track';
import {createView} from '../../trace_processor/sql_utils';

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

export abstract class CustomSqlTableSliceTrack<
  T extends NamedSliceTrackTypes,
> extends NamedSliceTrack<T> {
  protected readonly tableName;

  constructor(args: NewTrackArgs) {
    super(args);
    this.tableName = `customsqltableslicetrack_${uuidv4()
      .split('-')
      .join('_')}`;
  }

  abstract getSqlDataSource():
    | CustomSqlTableDefConfig
    | Promise<CustomSqlTableDefConfig>;

  // Override by subclasses.
  abstract getDetailsPanel(
    args: OnSliceClickArgs<NamedSliceTrackTypes['slice']>,
  ): CustomSqlDetailsPanelConfig;

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

  isSelectionHandled(selection: LegacySelection) {
    if (selection.kind !== 'GENERIC_SLICE') {
      return false;
    }
    return selection.trackKey === this.trackKey;
  }

  onSliceClick(args: OnSliceClickArgs<NamedSliceTrackTypes['slice']>) {
    if (this.getDetailsPanel(args) === undefined) {
      return;
    }

    const detailsPanelConfig = this.getDetailsPanel(args);
    globals.makeSelection(
      Actions.selectGenericSlice({
        id: args.slice.id,
        sqlTableName: this.tableName,
        start: args.slice.ts,
        duration: args.slice.dur,
        trackKey: this.trackKey,
        detailsPanelConfig: {
          kind: detailsPanelConfig.kind,
          config: detailsPanelConfig.config,
        },
      }),
    );
  }

  async loadImports() {
    for (const importModule of this.getSqlImports().modules) {
      await this.engine.query(`INCLUDE PERFETTO MODULE ${importModule};`);
    }
  }
}
