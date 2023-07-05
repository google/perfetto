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
import {
  PrimaryTrackSortKey,
  SCROLLING_TRACK_GROUP,
} from '../../common/state';
import {NamedSliceTrackTypes} from '../../frontend/named_slice_track';
import {NewTrackArgs, Track} from '../../frontend/track';
import {
  CustomSqlDetailsPanelConfig,
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../custom_sql_table_slices';

import {ScrollJankPluginState} from './index';
import {ScrollJankTracks as DecideTracksResult} from './index';
import {
  JankyEventLatenciesDetailsPanel,
} from './top_level_janky_event_latencies_details_panel';

export class TopLevelEventLatencyTrack extends
    CustomSqlTableSliceTrack<NamedSliceTrackTypes> {
  static readonly kind = 'org.chromium.ScrollJank.top_level_event_latencies';

  static create(args: NewTrackArgs): Track {
    return new TopLevelEventLatencyTrack(args);
  }

  constructor(args: NewTrackArgs) {
    super(args);
    ScrollJankPluginState.getInstance().registerTrack({
      kind: TopLevelEventLatencyTrack.kind,
      trackId: this.trackId,
      tableName: this.tableName,
      detailsPanelConfig: this.getDetailsPanel(),
    });
  }

  getSqlDataSource(): CustomSqlTableDefConfig {
    return {
      columns: [
        `id`,
        `ts`,
        `dur`,
        `track_id`,
        `IIF(
          cause_of_jank IS NOT NULL,
          cause_of_jank || IIF(
            sub_cause_of_jank IS NOT NULL, "::" || sub_cause_of_jank, ""
            ), "UNKNOWN") AS name`,
        `IFNULL(cause_of_jank, "UNKNOWN") AS jank_cause`,
        `name AS type`,
        `sub_cause_of_jank AS jank_subcause`,
      ],
      sqlTableName: 'chrome_janky_event_latencies_v3',
    };
  }

  getDetailsPanel(): CustomSqlDetailsPanelConfig {
    return {
      kind: JankyEventLatenciesDetailsPanel.kind,
      config: {
        sqlTableName: this.tableName,
        title: 'Chrome Scroll Jank Event Latency: Cause',
      },
    };
  }

  onDestroy() {
    super.onDestroy();
    ScrollJankPluginState.getInstance().unregisterTrack(
        TopLevelEventLatencyTrack.kind);
  }
}

export async function addJankyLatenciesTrack(engine: Engine):
    Promise<DecideTracksResult> {
  const result: DecideTracksResult = {
    tracksToAdd: [],
  };

  await engine.query(`SELECT IMPORT('chrome.chrome_scroll_janks');`);

  result.tracksToAdd.push({
    id: uuidv4(),
    engineId: engine.id,
    kind: TopLevelEventLatencyTrack.kind,
    trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
    name: 'Chrome Scroll Jank Event Latencies',
    config: {},
    trackGroup: SCROLLING_TRACK_GROUP,
  });

  return result;
}
