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
  generateSqlWithInternalLayout,
} from '../../common/internal_layout_utils';
import {PrimaryTrackSortKey, SCROLLING_TRACK_GROUP} from '../../common/state';
import {NamedSliceTrack} from '../../frontend/named_slice_track';
import {NewTrackArgs, Track} from '../../frontend/track';
import {DecideTracksResult} from '../chrome_scroll_jank';

export class EventLatencyTrack extends NamedSliceTrack {
  static readonly kind = 'org.chromium.ScrollJank.event_latencies';

  static create(args: NewTrackArgs): Track {
    return new EventLatencyTrack(args);
  }

  constructor(args: NewTrackArgs) {
    super(args);
  }

  async initSqlTable(tableName: string) {
    const sql =
      `CREATE VIEW ${tableName} AS ` +
      generateSqlWithInternalLayout({
        columns: ['id', 'ts', 'dur', 'track_id', 'cause_of_jank AS name'],
        layoutParams: {ts: 'ts', dur: 'dur'},
        sourceTable: 'chrome_janky_event_latencies_v2',
      }) + `;`;

    await this.engine.query(sql);
  }

  // At the moment we will just display the slice details. However, on select,
  // this behavior should be customized to show jank-related data.
}

export async function addLatenciesTrack(engine: Engine):
    Promise<DecideTracksResult> {
  const result: DecideTracksResult = {
    tracksToAdd: [],
  };

  await engine.query(`SELECT IMPORT('chrome.chrome_scroll_janks');`);

  result.tracksToAdd.push({
    id: uuidv4(),
    engineId: engine.id,
    kind: EventLatencyTrack.kind,
    trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
    name: 'Scroll Janks',
    config: {},
    trackGroup: SCROLLING_TRACK_GROUP,
  });

  return result;
}
