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
