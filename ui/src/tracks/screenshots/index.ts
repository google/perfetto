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

import {AddTrackArgs} from '../../common/actions';
import {Engine} from '../../common/engine';
import {
  NamedSliceTrackTypes,
} from '../../frontend/named_slice_track';
import {NewTrackArgs, Track} from '../../frontend/track';
import {
  Plugin,
  PluginContext,
  PluginInfo,
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

export {Data} from '../chrome_slices';

class ScreenshotsTrack extends CustomSqlTableSliceTrack<NamedSliceTrackTypes> {
  static readonly kind = 'dev.perfetto.ScreenshotsTrack';
  static create(args: NewTrackArgs): Track {
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

export async function decideTracks(engine: Engine):
    Promise<DecideTracksResult> {
  const result: DecideTracksResult = {
    tracksToAdd: [],
  };

  await engine.query(`SELECT IMPORT('android.screenshots')`);

  result.tracksToAdd.push({
    id: uuidv4(),
    engineId: engine.id,
    kind: ScreenshotsTrack.kind,
    trackSortKey: PrimaryTrackSortKey.ASYNC_SLICE_TRACK,
    name: 'Screenshots',
    config: {},
    trackGroup: undefined,
  });
  return result;
}

class ScreenshotsPlugin implements Plugin {
  onActivate(ctx: PluginContext): void {
    ctx.registerTrack(ScreenshotsTrack);
  }
}

export const plugin: PluginInfo = {
  pluginId: 'perfetto.Screenshots',
  plugin: ScreenshotsPlugin,
};
