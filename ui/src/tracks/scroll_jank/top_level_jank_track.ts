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
import {PrimaryTrackSortKey, SCROLLING_TRACK_GROUP} from '../../common/state';
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

interface TopLevelJankTrackTypes extends NamedSliceTrackTypes {
  config: {sqlTableName: string;}
}

export class TopLevelJankTrack extends
    CustomSqlTableSliceTrack<TopLevelJankTrackTypes> {
  static readonly kind = 'org.chromium.ScrollJank.top_level_jank';
  displayColumns: Columns = {};

  static create(args: NewTrackArgs): Track {
    return new TopLevelJankTrack(args);
  }

  constructor(args: NewTrackArgs) {
    super(args);

    this.displayColumns['name'] = {};
    this.displayColumns['id'] = {displayName: 'Interval ID'};
    this.displayColumns['ts'] = {displayName: 'Start time'};
    this.displayColumns['dur'] = {displayName: 'Duration'};
  }

  getSqlDataSource(): CustomSqlTableDefConfig {
    return {
      sqlTableName: this.config.sqlTableName,
    };
  }

  getDetailsPanel(): CustomSqlDetailsPanelConfig {
    return {
      kind: GenericSliceDetailsTab.kind,
      config: {
        sqlTableName: this.tableName,
        title: 'Chrome Scroll Jank Summary',
        columns: this.displayColumns,
      },
    };
  }

  async initSqlTable(tableName: string) {
    await super.initSqlTable(tableName);
  }
}

export async function addTopLevelJankTrack(engine: Engine):
    Promise<DecideTracksResult> {
  const result: DecideTracksResult = {
    tracksToAdd: [],
  };

  const viewName =
      `view_${uuidv4().split('-').join('_')}_top_level_scrolls_jank_summary`;
  const viewDefSql = `CREATE VIEW ${viewName} AS 
      WITH unioned_data AS (
      SELECT
        "Scrolling: " || scroll_ids AS name,
        ts,
        dur
      FROM chrome_scrolling_intervals
      UNION ALL
      SELECT
        "Janky Scrolling Time" AS name,
        ts,
        dur
      FROM chrome_scroll_jank_intervals_v2
     )
     SELECT
       ROW_NUMBER() OVER(ORDER BY ts) AS id,
       *
     FROM unioned_data;`;

  await engine.query(`SELECT IMPORT('chrome.chrome_scrolls')`);
  await engine.query(`SELECT IMPORT('chrome.chrome_scroll_janks')`);
  await engine.query(viewDefSql);

  result.tracksToAdd.push({
    id: uuidv4(),
    engineId: engine.id,
    kind: TopLevelJankTrack.kind,
    trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
    name: 'Chrome Scroll Jank Summary',
    config: {
      sqlTableName: viewName,
    },
    trackGroup: SCROLLING_TRACK_GROUP,
  });

  return result;
}
