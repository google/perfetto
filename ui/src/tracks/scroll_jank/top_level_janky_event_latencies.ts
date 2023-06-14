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
import {
  Columns,
  GenericSliceDetailsTab,
} from '../../frontend/generic_slice_details_tab';
import {NamedSliceTrackTypes} from '../../frontend/named_slice_track';
import {NewTrackArgs, Track} from '../../frontend/track';
import {DecideTracksResult} from '../chrome_scroll_jank';
import {
  CustomSqlDetailsPanelConfig,
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../custom_sql_table_slices';

export class TopLevelEventLatencyTrack extends
    CustomSqlTableSliceTrack<NamedSliceTrackTypes> {
  static readonly kind = 'org.chromium.ScrollJank.top_level_event_latencies';
  displayColumns: Columns = {};

  static create(args: NewTrackArgs): Track {
    return new TopLevelEventLatencyTrack(args);
  }

  constructor(args: NewTrackArgs) {
    super(args);
    this.displayColumns['cause_of_jank'] = {displayName: 'Cause of Jank'};
    this.displayColumns['sub_cause_of_jank'] = {
      displayName: 'Sub-cause of Jank',
    };
    this.displayColumns['id'] = {displayName: 'Slice ID'};
    this.displayColumns['ts'] = {displayName: 'Start time'};
    this.displayColumns['dur'] = {displayName: 'Duration'};
    this.displayColumns['type'] = {displayName: 'Slice Type'};
  }

  getSqlDataSource(): CustomSqlTableDefConfig {
    return {
      columns: [
        'id',
        'ts',
        'dur',
        'track_id',
        'cause_of_jank || IIF(sub_cause_of_jank IS NOT NULL, "::" || sub_cause_of_jank, "") AS name',
        'cause_of_jank',
        'name AS type',
        'sub_cause_of_jank',
      ],
      sqlTableName: 'chrome_janky_event_latencies_v2',
    };
  }

  getDetailsPanel(): CustomSqlDetailsPanelConfig {
    return {
      kind: GenericSliceDetailsTab.kind,
      config: {
        sqlTableName: this.tableName,
        title: 'Chrome Scroll Jank Event Latency: Cause',
        columns: this.displayColumns,
      },
    };
  }

  async initSqlTable(tableName: string) {
    super.initSqlTable(tableName);
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
