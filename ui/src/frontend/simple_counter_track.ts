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

import {TrackContext} from '../public/track';
import {BaseCounterTrack, CounterOptions} from './base_counter_track';
import {
  CounterColumns,
  SqlDataSource,
} from '../public/lib/debug_tracks/debug_tracks';
import {uuidv4Sql} from '../base/uuid';
import {createPerfettoTable} from '../trace_processor/sql_utils';
import {Trace} from '../public/trace';

export type SimpleCounterTrackConfig = {
  data: SqlDataSource;
  columns: CounterColumns;
  options?: Partial<CounterOptions>;
};

export class SimpleCounterTrack extends BaseCounterTrack {
  private config: SimpleCounterTrackConfig;
  private sqlTableName: string;

  constructor(
    trace: Trace,
    ctx: TrackContext,
    config: SimpleCounterTrackConfig,
  ) {
    super({
      trace,
      uri: ctx.trackUri,
      options: config.options,
    });
    this.config = config;
    this.sqlTableName = `__simple_counter_${uuidv4Sql()}`;
  }

  async onInit() {
    return await createPerfettoTable(
      this.engine,
      this.sqlTableName,
      `
        with data as (
          ${this.config.data.sqlSource}
        )
        select
          ${this.config.columns.ts} as ts,
          ${this.config.columns.value} as value
        from data
        order by ts
      `,
    );
  }

  getSqlSource(): string {
    return `select * from ${this.sqlTableName}`;
  }
}
