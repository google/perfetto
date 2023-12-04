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

import {NamedSliceTrackTypes} from '../../frontend/named_slice_track';
import {NewTrackArgs, TrackBase} from '../../frontend/track';
import {PrimaryTrackSortKey} from '../../public';
import {
  CustomSqlDetailsPanelConfig,
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../custom_sql_table_slices';
import {
  SCROLL_JANK_GROUP_ID,
  ScrollJankPluginState,
  ScrollJankTracks as DecideTracksResult,
} from './index';
import {ScrollDetailsPanel} from './scroll_details_panel';

export const CHROME_TOPLEVEL_SCROLLS_KIND =
    'org.chromium.TopLevelScrolls.scrolls';

export class TopLevelScrollTrack extends
    CustomSqlTableSliceTrack<NamedSliceTrackTypes> {
  public static kind = CHROME_TOPLEVEL_SCROLLS_KIND;
  static create(args: NewTrackArgs): TrackBase {
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
      trackKey: this.trackKey,
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

export async function addTopLevelScrollTrack(): Promise<DecideTracksResult> {
  const result: DecideTracksResult = {
    tracksToAdd: [],
  };

  result.tracksToAdd.push({
    uri: 'perfetto.ChromeScrollJank#toplevelScrolls',
    trackSortKey: PrimaryTrackSortKey.ASYNC_SLICE_TRACK,
    name: 'Chrome Scrolls',
    trackGroup: SCROLL_JANK_GROUP_ID,
  });

  return result;
}
