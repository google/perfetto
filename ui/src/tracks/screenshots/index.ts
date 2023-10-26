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

import {AddTrackArgs} from '../../common/actions';
import {Engine} from '../../common/engine';
import {
  NamedSliceTrackTypes,
} from '../../frontend/named_slice_track';
import {NewTrackArgs, TrackBase} from '../../frontend/track';
import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
  PrimaryTrackSortKey,
} from '../../public';
import {
  CustomSqlDetailsPanelConfig,
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../custom_sql_table_slices';

import {
  ScreenshotTab,
} from './screenshot_panel';

class ScreenshotsTrack extends CustomSqlTableSliceTrack<NamedSliceTrackTypes> {
  static readonly kind = 'dev.perfetto.ScreenshotsTrack';
  static create(args: NewTrackArgs): TrackBase {
    return new ScreenshotsTrack(args);
  }

  getSqlDataSource(): CustomSqlTableDefConfig {
    return {
      sqlTableName: 'android_screenshots',
      columns: ['*'],
    };
  }

  getDetailsPanel(): CustomSqlDetailsPanelConfig {
    return {
      kind: ScreenshotTab.kind,
      config: {
        sqlTableName: this.tableName,
        title: 'Screenshots',
      },
    };
  }
}

export type DecideTracksResult = {
  tracksToAdd: AddTrackArgs[],
};

// TODO(stevegolton): Use suggestTrack().
export async function decideTracks(engine: Engine):
    Promise<DecideTracksResult> {
  const result: DecideTracksResult = {
    tracksToAdd: [],
  };

  await engine.query(`INCLUDE PERFETTO MODULE android.screenshots`);

  result.tracksToAdd.push({
    uri: 'perfetto.Screenshots',
    name: 'Screenshots',
    trackSortKey: PrimaryTrackSortKey.ASYNC_SLICE_TRACK,
  });
  return result;
}

class ScreenshotsPlugin implements Plugin {
  onActivate(_ctx: PluginContext): void {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    ctx.registerStaticTrack({
      uri: 'perfetto.Screenshots',
      displayName: 'Screenshots',
      kind: ScreenshotsTrack.kind,
      track: ({trackKey}) => {
        return new ScreenshotsTrack({
          engine: ctx.engine,
          trackKey,
        });
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.Screenshots',
  plugin: ScreenshotsPlugin,
};
