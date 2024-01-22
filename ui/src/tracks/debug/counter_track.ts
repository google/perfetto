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
import {v4 as uuidv4} from 'uuid';

import {Actions, DeferredAction} from '../../common/actions';
import {SCROLLING_TRACK_GROUP} from '../../common/state';
import {BaseCounterTrack} from '../../frontend/base_counter_track';
import {globals} from '../../frontend/globals';
import {TrackButton} from '../../frontend/track_panel';
import {PrimaryTrackSortKey, TrackContext} from '../../public';
import {EngineProxy} from '../../trace_processor/engine';

import {DEBUG_COUNTER_TRACK_URI} from '.';

// Names of the columns of the underlying view to be used as ts / dur / name.
export interface CounterColumns {
  ts: string;
  value: string;
}

export interface CounterDebugTrackConfig {
  sqlTableName: string;
  columns: CounterColumns;
  closeable: boolean;
}

export interface CounterDebugTrackCreateConfig {
  pinned?: boolean;     // default true
  closeable?: boolean;  // default true
}

export class DebugCounterTrack extends BaseCounterTrack {
  private config: CounterDebugTrackConfig;

  constructor(engine: EngineProxy, ctx: TrackContext) {
    super({
      engine,
      trackKey: ctx.trackKey,
    });

    // TODO(stevegolton): Validate params before type asserting.
    // TODO(stevegolton): Avoid just pushing this config up for some base
    // class to use. Be more explicit.
    this.config = ctx.params as CounterDebugTrackConfig;
  }

  getTrackShellButtons(): m.Children {
    return [
      this.getCounterContextMenu(),
      this.config.closeable && m(TrackButton, {
        action: () => {
          globals.dispatch(Actions.removeTracks({trackKeys: [this.trackKey]}));
        },
        i: 'close',
        tooltip: 'Close',
        showButton: true,
      }),
    ];
  }

  getSqlSource(): string {
    return `select * from ${this.config.sqlTableName}`;
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
    columns: CounterColumns,
    config?: CounterDebugTrackCreateConfig) {
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

  const closeable = config?.closeable ?? true;
  const trackKey = uuidv4();
  const actions: DeferredAction<{}>[] = [
    Actions.addTrack({
      key: trackKey,
      uri: DEBUG_COUNTER_TRACK_URI,
      name: trackName.trim() || `Debug Track ${debugTrackId}`,
      trackSortKey: PrimaryTrackSortKey.DEBUG_TRACK,
      trackGroup: SCROLLING_TRACK_GROUP,
      params: {
        sqlTableName,
        columns,
        closeable,
      },
    }),
  ];
  if (config?.pinned ?? true) {
    actions.push(Actions.toggleTrackPinned({trackKey}));
  }
  globals.dispatchMultiple(actions);
}
