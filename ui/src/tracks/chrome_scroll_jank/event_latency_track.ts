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
  NamedSliceTrackTypes,
} from '../../frontend/named_slice_track';
import {NewTrackArgs, Track} from '../../frontend/track';
import {
  CustomSqlDetailsPanelConfig,
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../custom_sql_table_slices';

import {EventLatencySliceDetailsPanel} from './event_latency_details_panel';
import {ScrollJankTracks as DecideTracksResult} from './index';
import {ScrollJankPluginState} from './index';

export interface EventLatencyTrackTypes extends NamedSliceTrackTypes {
  config: {baseTable: string;}
}

export class EventLatencyTrack extends
    CustomSqlTableSliceTrack<EventLatencyTrackTypes> {
  static readonly kind = 'org.chromium.ScrollJank.event_latencies';

  static create(args: NewTrackArgs): Track {
    return new EventLatencyTrack(args);
  }

  constructor(args: NewTrackArgs) {
    super(args);
    ScrollJankPluginState.getInstance().registerTrack({
      kind: EventLatencyTrack.kind,
      trackId: this.trackId,
      tableName: this.tableName,
      detailsPanelConfig: this.getDetailsPanel(),
    });
  }

  onDestroy() {
    super.onDestroy();
    ScrollJankPluginState.getInstance().unregisterTrack(EventLatencyTrack.kind);
  }

  async initSqlTable(tableName: string) {
    const sql =
        `CREATE VIEW ${tableName} AS SELECT * FROM ${this.config.baseTable}`;

    await this.engine.query(sql);
  }

  getDetailsPanel(): CustomSqlDetailsPanelConfig {
    return {
      kind: EventLatencySliceDetailsPanel.kind,
      config: {title: '', sqlTableName: this.tableName},
    };
  }

  getSqlDataSource(): CustomSqlTableDefConfig {
    return {
      sqlTableName: this.config.baseTable,
    };
  }

  // At the moment we will just display the slice details. However, on select,
  // this behavior should be customized to show jank-related data.
}

export async function addLatencyTracks(engine: Engine):
    Promise<DecideTracksResult> {
  const result: DecideTracksResult = {
    tracksToAdd: [],
  };

  const subTableSql = generateSqlWithInternalLayout({
    columns: ['id', 'ts', 'dur', 'track_id', 'name'],
    sourceTable: 'slice',
    ts: 'ts',
    dur: 'dur',
    whereClause: `
      EXTRACT_ARG(arg_set_id, 'event_latency.event_type') IN (
        'FIRST_GESTURE_SCROLL_UPDATE',
        'GESTURE_SCROLL_UPDATE',
        'INERTIAL_GESTURE_SCROLL_UPDATE')
      AND HAS_DESCENDANT_SLICE_WITH_NAME(
        id,
        'SubmitCompositorFrameToPresentationCompositorFrame')`,
  });

  // Table name must be unique - it cannot include '-' characters or begin with
  // a numeric value.
  const baseTable =
      `table_${uuidv4().split('-').join('_')}_janky_event_latencies_v3`;
  const tableDefSql = `CREATE TABLE ${baseTable} AS
      WITH event_latencies AS (
        ${subTableSql}
      ), latency_stages AS (
      SELECT
        d.id,
        d.ts,
        d.dur,
        d.track_id,
        d.name,
        d.depth,
        min(a.id) AS parent_id
      FROM slice s
        JOIN descendant_slice(s.id) d
        JOIN ancestor_slice(d.id) a
      WHERE s.id IN (SELECT id FROM event_latencies)
      GROUP BY d.id, d.ts, d.dur, d.track_id, d.name, d.parent_id, d.depth)
    SELECT
      id,
      ts,
      dur,
      CASE
        WHEN id IN (
          SELECT id FROM chrome_janky_event_latencies_v3)
        THEN 'Janky EventLatency'
        ELSE name
      END
      AS name,
      depth * 3 AS depth
    FROM event_latencies
    UNION ALL
    SELECT
      ls.id,
      ls.ts,
      ls.dur,
      ls.name,
      depth + (
        (SELECT depth FROM event_latencies
        WHERE id = ls.parent_id LIMIT 1) * 3) AS depth
    FROM latency_stages ls;`;

  await engine.query(`SELECT IMPORT('chrome.chrome_scroll_janks')`);
  await engine.query(tableDefSql);

  result.tracksToAdd.push({
    id: uuidv4(),
    engineId: engine.id,
    kind: EventLatencyTrack.kind,
    trackSortKey: PrimaryTrackSortKey.ASYNC_SLICE_TRACK,
    name: 'Chrome Scroll Input Latencies',
    config: {baseTable: baseTable},
    trackGroup: SCROLLING_TRACK_GROUP,
  });

  return result;
}
