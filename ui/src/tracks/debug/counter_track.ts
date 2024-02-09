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

import {Actions} from '../../common/actions';
import {BaseCounterTrack} from '../../frontend/base_counter_track';
import {globals} from '../../frontend/globals';
import {TrackButton} from '../../frontend/track_panel';
import {TrackContext} from '../../public';
import {EngineProxy} from '../../trace_processor/engine';
import {CounterDebugTrackConfig} from '../../frontend/debug_tracks';
import {Disposable, DisposableCallback} from '../../base/disposable';
import {uuidv4Sql} from '../../base/uuid';


export class DebugCounterTrack extends BaseCounterTrack {
  private config: CounterDebugTrackConfig;
  private sqlTableName: string;

  constructor(engine: EngineProxy, ctx: TrackContext) {
    super({
      engine,
      trackKey: ctx.trackKey,
    });

    // TODO(stevegolton): Validate params before type asserting.
    // TODO(stevegolton): Avoid just pushing this config up for some base
    // class to use. Be more explicit.
    this.config = ctx.params as CounterDebugTrackConfig;
    this.sqlTableName = `__debug_counter_${uuidv4Sql(this.trackKey)}`;
  }

  async onInit(): Promise<Disposable> {
    await this.createTrackTable();
    return new DisposableCallback(() => {
      this.dropTrackTable();
    });
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
    return `select * from ${this.sqlTableName}`;
  }

  private async createTrackTable(): Promise<void> {
    await this.engine.query(`
        create table ${this.sqlTableName} as
        with data as (
          ${this.config.data.sqlSource}
        )
        select
          ${this.config.columns.ts} as ts,
          ${this.config.columns.value} as value
        from data
        order by ts;`);
  }

  private async dropTrackTable(): Promise<void> {
    if (this.engine.isAlive) {
      this.engine.query(`drop table if exists ${this.sqlTableName}`);
    }
  }
}
