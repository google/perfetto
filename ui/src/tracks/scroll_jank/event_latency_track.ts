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
import {
  NamedSliceTrack,
  NamedSliceTrackTypes,
} from '../../frontend/named_slice_track';
import {NewTrackArgs, Track} from '../../frontend/track';
import {DecideTracksResult} from '../chrome_scroll_jank';

interface EventLatencyTrackTypes extends NamedSliceTrackTypes {}

export class EventLatencyTrack extends NamedSliceTrack<EventLatencyTrackTypes> {
  static readonly kind = 'org.chromium.ScrollJank.event_latencies';
  createdModels = false;

  static create(args: NewTrackArgs): Track {
    return new EventLatencyTrack(args);
  }

  constructor(args: NewTrackArgs) {
    super(args);
  }

  async initSqlTable(tableName: string) {
    if (this.createdModels) {
      return;
    }
    const sql = `CREATE VIEW ${tableName} AS ` +
        `SELECT * FROM _perfetto_ui_impl_chrome_event_latency_scroll_janks`;
    await this.engine.query(sql);
    this.createdModels = true;
  }

  // At the moment we will just display the slice details. However, on select,
  // this behavior should be customized to show jank-related data.
}

export async function addLatenciesTrack(engine: Engine):
    Promise<DecideTracksResult> {
  const result: DecideTracksResult = {
    tracksToAdd: [],
  };

  await engine.query(`
      SELECT RUN_METRIC('chrome/event_latency_scroll_jank_cause.sql');
    `);

  const sql =
      `CREATE TABLE _perfetto_ui_impl_chrome_event_latency_scroll_janks AS ` +
      generateSqlWithInternalLayout({
        columns: ['id', 'ts', 'dur', 'track_id', 'name'],
        layoutParams: {ts: 'ts', dur: 'dur'},
        sourceTable: 'slice',
        whereClause: 'slice.id IN ' +
            '(SELECT slice_id FROM event_latency_scroll_jank_cause)',
      });

  await engine.query(sql);

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
