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
import {globals} from '../../frontend/globals';
import {NamedSliceTrackTypes} from '../../frontend/named_slice_track';
import {TrackButton} from '../../frontend/track_panel';
import {TrackContext} from '../../public';
import {EngineProxy} from '../../trace_processor/engine';
import {
  CustomSqlDetailsPanelConfig,
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../custom_sql_table_slices';

import {DebugSliceDetailsTab} from './details_tab';
import {SliceColumns} from '../../frontend/debug_tracks';

export interface DebugTrackV2Config {
  sqlTableName: string;
  columns: SliceColumns;
  closeable: boolean;
}


export class DebugTrackV2 extends
  CustomSqlTableSliceTrack<NamedSliceTrackTypes> {
  private config: DebugTrackV2Config;

  constructor(engine: EngineProxy, ctx: TrackContext) {
    super({
      engine,
      trackKey: ctx.trackKey,
    });

    // TODO(stevegolton): Validate params before type asserting.
    // TODO(stevegolton): Avoid just pushing this config up for some base
    // class to use. Be more explicit.
    this.config = ctx.params as DebugTrackV2Config;
  }

  getSqlDataSource(): CustomSqlTableDefConfig {
    return {
      sqlTableName: this.config!.sqlTableName,
    };
  }

  getDetailsPanel(): CustomSqlDetailsPanelConfig {
    return {
      kind: DebugSliceDetailsTab.kind,
      config: {
        sqlTableName: this.config!.sqlTableName,
        title: 'Debug Slice',
      },
    };
  }

  getTrackShellButtons(): m.Children {
    return this.config.closeable ? m(TrackButton, {
      action: () => {
        globals.dispatch(Actions.removeTracks({trackKeys: [this.trackKey]}));
      },
      i: 'close',
      tooltip: 'Close',
      showButton: true,
    }) :
      [];
  }
}

