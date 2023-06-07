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

import {NamedSliceTrack, NamedSliceTrackTypes} from '../../frontend/named_slice_track';
import {NewTrackArgs, Track} from '../../frontend/track';
import {PrimaryTrackSortKey, SCROLLING_TRACK_GROUP, Selection} from '../../common/state';
import {OnSliceClickArgs} from '../../frontend/base_slice_track';
import {globals} from '../../frontend/globals';
import {Actions} from '../../common/actions';
import {Engine} from '../../common/engine';
import {DecideTracksResult} from '../chrome_scroll_jank';
import {v4 as uuidv4} from 'uuid';
import {GenericSliceDetailsTab, Columns} from '../../frontend/generic_slice_details_tab';

export class TopLevelJankTrack extends NamedSliceTrack {
  static readonly kind = 'org.chromium.ScrollJank.top_level_jank';

  static create(args: NewTrackArgs): Track {
    return new TopLevelJankTrack(args);
  }

  constructor(args: NewTrackArgs) {
    super(args);
  }

  async initSqlTable(tableName: string) {
    const sql = `CREATE VIEW ${tableName} AS
    WITH unioned_data AS (
      SELECT
        "Scrolling" AS name,
        ts,
        dur,
        0 AS depth
      FROM chrome_scrolling_intervals
      UNION ALL
      SELECT
        "Janky Scrolling Time" AS name,
        ts,
        dur,
        1 AS depth
      FROM chrome_scroll_jank_intervals_v2
     )
     SELECT
       ROW_NUMBER() OVER(ORDER BY ts) AS id,
       *
     FROM unioned_data
    `;
    await this.engine.query(sql);
  }

  isSelectionHandled(selection: Selection) {
    if (selection.kind !== 'BASIC_SQL_OBJECT') {
      return false;
    }
    return selection.trackId === this.trackId;
  }

  onSliceClick(args: OnSliceClickArgs<NamedSliceTrackTypes['slice']>) {
    const columns : Columns = {};
    columns['name'] = {};
    columns['id'] = {displayName: 'Interval ID'};
    columns['ts'] = {displayName: 'Start time'};
    columns['dur'] = {displayName: 'Duration'};

    const title = 'Scroll Jank Summary';

    globals.dispatch(Actions.selectBasicSqlSlice({
      id: args.slice.id,
      sqlTableName: this.tableName,
      start: args.slice.start,
      duration: args.slice.duration,
      trackId: this.trackId,
      detailsPanelConfig: {
        kind: GenericSliceDetailsTab.kind,
        config: {
          id: args.slice.id,
          sqlTableName: this.tableName,
          title: title,
          columns: columns,
        },
      },
    }));
  }
}

export async function addTopLevelJankTrack(engine: Engine):
    Promise<DecideTracksResult> {
  const result: DecideTracksResult = {
    tracksToAdd: [],
  };

  await engine.query(`SELECT IMPORT('chrome.chrome_scroll_janks');`);

  result.tracksToAdd.push({
    id: uuidv4(),
    engineId: engine.id,
    kind: TopLevelJankTrack.kind,
    trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
    name: 'Scroll Jank Summary',
    config: {},
    trackGroup: SCROLLING_TRACK_GROUP,
  });

  return result;
}

