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

import m from 'mithril';

import {Actions, DEBUG_COUNTER_TRACK_KIND} from '../../common/actions';
import {EngineProxy} from '../../common/engine';
import {BaseCounterTrack} from '../../frontend/base_counter_track';
import {globals} from '../../frontend/globals';
import {NewTrackArgs} from '../../frontend/track';
import {TrackButton, TrackButtonAttrs} from '../../frontend/track_panel';

// Names of the columns of the underlying view to be used as ts / dur / name.
export interface CounterColumns {
  ts: string;
  value: string;
}

export interface CounterDebugTrackConfig {
  sqlTableName: string;
  columns: CounterColumns;
}

export class DebugCounterTrack extends
    BaseCounterTrack<CounterDebugTrackConfig> {
  static readonly kind = DEBUG_COUNTER_TRACK_KIND;

  static create(args: NewTrackArgs) {
    return new DebugCounterTrack(args);
  }

  constructor(args: NewTrackArgs) {
    super(args);
  }

  getTrackShellButtons(): Array<m.Vnode<TrackButtonAttrs>> {
    return [m(TrackButton, {
      action: () => {
        globals.dispatch(Actions.removeDebugTrack({trackId: this.trackId}));
      },
      i: 'close',
      tooltip: 'Close',
      showButton: true,
    })];
  }

  async initSqlTable(tableName: string): Promise<void> {
    await this.engine.query(`
      create view ${tableName} as
      select * from ${this.config.sqlTableName};
    `);
  }
}

let debugTrackCount = 0;

export interface SqlDataSource {
  // SQL source selecting the necessary data.
  sqlSource: string;
  // The caller is responsible for ensuring that the number of items in this
  // list matches the number of columns returned by sqlSource.
  columns: string[];
}

export async function addDebugCounterTrack(
    engine: EngineProxy,
    data: SqlDataSource,
    trackName: string,
    columns: CounterColumns) {
  // To prepare displaying the provided data as a track, materialize it and
  // compute depths.
  const debugTrackId = ++debugTrackCount;
  const sqlTableName = `__debug_counter_${debugTrackId}`;

  // TODO(altimin): Support removing this table when the track is closed.
  await engine.query(`
      create table ${sqlTableName} as
      with data as (
        ${data.sqlSource}
      )
      select
        ${columns.ts} as ts,
        ${columns.value} as value
      from data
      order by ts;`);

  globals.dispatch(Actions.addDebugCounterTrack({
    engineId: engine.engineId,
    name: trackName.trim() || `Debug Track ${debugTrackId}`,
    config: {
      sqlTableName,
      columns,
    },
  }));
}
