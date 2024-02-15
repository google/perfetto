// Copyright (C) 2024 The Android Open Source Project
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

import {EngineProxy, TrackContext} from '../public';
import {CustomSqlDetailsPanelConfig, CustomSqlTableDefConfig, CustomSqlTableSliceTrack} from '../tracks/custom_sql_table_slices';
import {NamedSliceTrackTypes} from './named_slice_track';
import {ARG_PREFIX, SliceColumns, SqlDataSource} from './debug_tracks';
import {uuidv4Sql} from '../base/uuid';
import {DisposableCallback} from '../base/disposable';
import {DebugSliceDetailsTab} from '../tracks/debug/details_tab';

export interface SimpleSliceTrackConfig {
  data: SqlDataSource;
  columns: SliceColumns;
  argColumns: string[];
}

export class SimpleSliceTrack extends
  CustomSqlTableSliceTrack<NamedSliceTrackTypes> {
  private config: SimpleSliceTrackConfig;
  private sqlTableName: string;

  constructor(
    engine: EngineProxy,
    ctx: TrackContext,
    config: SimpleSliceTrackConfig) {
    super({
      engine,
      trackKey: ctx.trackKey,
    });

    this.config = config;
    this.sqlTableName = `__simple_slice_${uuidv4Sql(ctx.trackKey)}`;
  }

  async getSqlDataSource(): Promise<CustomSqlTableDefConfig> {
    await this.createTrackTable(
      this.config.data,
      this.config.columns,
      this.config.argColumns,
    );
    return {
      sqlTableName: this.sqlTableName,
      dispose: new DisposableCallback(() => this.destroyTrackTable()),
    };
  }

  getDetailsPanel(): CustomSqlDetailsPanelConfig {
    // We currently borrow the debug slice details tab.
    // TODO: Don't do this!
    return {
      kind: DebugSliceDetailsTab.kind,
      config: {
        sqlTableName: this.sqlTableName,
        title: 'Debug Slice',
      },
    };
  }

  private async createTrackTable(
    data: SqlDataSource,
    sliceColumns: SliceColumns,
    argColumns: string[]): Promise<void> {
    // If the view has clashing names (e.g. "name" coming from joining two
    // different tables, we will see names like "name_1", "name_2", but they
    // won't be addressable from the SQL. So we explicitly name them through a
    // list of columns passed to CTE.
    const dataColumns =
      data.columns !== undefined ? `(${data.columns.join(', ')})` : '';

    // TODO(altimin): Support removing this table when the track is closed.
    const dur = sliceColumns.dur === '0' ? 0 : sliceColumns.dur;
    await this.engine.query(`
      create table ${this.sqlTableName} as
      with data${dataColumns} as (
        ${data.sqlSource}
      ),
      prepared_data as (
        select
          row_number() over () as id,
          ${sliceColumns.ts} as ts,
          ifnull(cast(${dur} as int), -1) as dur,
          printf('%s', ${sliceColumns.name}) as name
          ${argColumns.length > 0 ? ',' : ''}
          ${argColumns.map((c) => `${c} as ${ARG_PREFIX}${c}`).join(',\n')}
        from data
      )
      select
        *
      from prepared_data
      order by ts;`);
  }

  private async destroyTrackTable() {
    if (this.engine.isAlive) {
      await this.engine.query(`DROP TABLE IF EXISTS ${this.sqlTableName}`);
    }
  }
}
