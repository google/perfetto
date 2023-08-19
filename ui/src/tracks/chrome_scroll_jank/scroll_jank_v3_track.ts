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

// import { v4 as uuidv4 } from 'uuid';

// import { Engine } from '../../common/engine';
// import {
//   PrimaryTrackSortKey,
//   SCROLLING_TRACK_GROUP,
// } from '../../common/state';
import {Engine} from '../../common/engine';
import {NamedSliceTrackTypes} from '../../frontend/named_slice_track';
import {NewTrackArgs, Track} from '../../frontend/track';
import {
  CustomSqlDetailsPanelConfig,
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../custom_sql_table_slices';

import {
  ScrollJankPluginState,
  ScrollJankTracks as DecideTracksResult,
} from './index';
import {ScrollJankV3DetailsPanel} from './scroll_jank_v3_details_panel';

export {Data} from '../chrome_slices';

export class ScrollJankV3Track extends
    CustomSqlTableSliceTrack<NamedSliceTrackTypes> {
  static readonly kind = 'org.chromium.ScrollJank.scroll_jank_v3_track';

  static create(args: NewTrackArgs): Track {
    return new ScrollJankV3Track(args);
  }

  constructor(args: NewTrackArgs) {
    super(args);

    ScrollJankPluginState.getInstance().registerTrack({
      kind: ScrollJankV3Track.kind,
      trackId: this.trackId,
      tableName: this.tableName,
      detailsPanelConfig: this.getDetailsPanel(),
    });
  }

  getSqlDataSource(): CustomSqlTableDefConfig {
    return {
      columns: [
        `IIF(
          cause_of_jank IS NOT NULL,
          cause_of_jank || IIF(
            sub_cause_of_jank IS NOT NULL, "::" || sub_cause_of_jank, ""
            ), "Unknown") || " Jank" AS name`,
        'id',
        'ts',
        'dur',
      ],
      sqlTableName: 'chrome_janky_frame_presentation_intervals',
    };
  }

  getDetailsPanel(): CustomSqlDetailsPanelConfig {
    return {
      kind: ScrollJankV3DetailsPanel.kind,
      config: {
        sqlTableName: 'chrome_janky_frame_presentation_intervals',
        title: 'Chrome Scroll Janks',
      },
    };
  }

  onDestroy() {
    super.onDestroy();
    ScrollJankPluginState.getInstance().unregisterTrack(ScrollJankV3Track.kind);
  }
}

export async function addScrollJankV3ScrollTrack(_engine: Engine):
    Promise<DecideTracksResult> {
  const result: DecideTracksResult = {
    tracksToAdd: [],
  };

  // TODO(b/296401533): Reenable.
  // await engine.query(`SELECT IMPORT('chrome.chrome_scroll_janks')`);

  // result.tracksToAdd.push({
  //   id: uuidv4(),
  //   engineId: engine.id,
  //   kind: ScrollJankV3Track.kind,
  //   trackSortKey: PrimaryTrackSortKey.ASYNC_SLICE_TRACK,
  //   name: 'Chrome Scroll Janks',
  //   config: {},
  //   trackGroup: SCROLLING_TRACK_GROUP,
  // });

  return result;
}
