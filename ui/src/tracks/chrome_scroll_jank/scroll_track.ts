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

import {Engine} from '../../common/engine';
import {SCROLLING_TRACK_GROUP} from '../../common/state';
import {NamedSliceTrackTypes} from '../../frontend/named_slice_track';
import {NewTrackArgs, Track} from '../../frontend/track';
import {PrimaryTrackSortKey} from '../../public';
import {
  CustomSqlDetailsPanelConfig,
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../custom_sql_table_slices';

import {
  ScrollJankPluginState,
  ScrollJankTracks as DecideTracksResult,
} from './index';
import {ScrollDetailsPanel} from './scroll_details_panel';

export {Data} from '../chrome_slices';

export class TopLevelScrollTrack extends
    CustomSqlTableSliceTrack<NamedSliceTrackTypes> {
  static readonly kind = 'org.chromium.TopLevelScrolls.scrolls';

  static create(args: NewTrackArgs): Track {
    return new TopLevelScrollTrack(args);
  }

  getSqlDataSource(): CustomSqlTableDefConfig {
    return {
      columns: [`printf("Scroll %s", CAST(id AS STRING)) AS name`, '*'],
      sqlTableName: 'chrome_scrolls',
    };
  }

  getDetailsPanel(): CustomSqlDetailsPanelConfig {
    return {
      kind: ScrollDetailsPanel.kind,
      config: {
        sqlTableName: this.tableName,
        title: 'Chrome Top Level Scrolls',
      },
    };
  }

  constructor(args: NewTrackArgs) {
    super(args);

    ScrollJankPluginState.getInstance().registerTrack({
      kind: TopLevelScrollTrack.kind,
      trackId: this.trackId,
      tableName: this.tableName,
      detailsPanelConfig: this.getDetailsPanel(),
    });
  }

  onDestroy() {
    super.onDestroy();
    ScrollJankPluginState.getInstance().unregisterTrack(
        TopLevelScrollTrack.kind);
  }
}

export async function addTopLevelScrollTrack(engine: Engine):
    Promise<DecideTracksResult> {
  const result: DecideTracksResult = {
    tracksToAdd: [],
  };

  await engine.query(`SELECT IMPORT('chrome.chrome_scrolls')`);

  result.tracksToAdd.push({
    id: uuidv4(),
    engineId: engine.id,
    kind: TopLevelScrollTrack.kind,
    trackSortKey: PrimaryTrackSortKey.ASYNC_SLICE_TRACK,
    name: 'Chrome Scrolls',
    config: {},
    trackGroup: SCROLLING_TRACK_GROUP,
  });

  return result;
}
