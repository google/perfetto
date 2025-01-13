// Copyright (C) 2022 The Android Open Source Project
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

import {uuidv4Sql} from '../../base/uuid';
import {generateSqlWithInternalLayout} from '../../components/sql_utils/layout';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {EventLatencyTrack, JANKY_LATENCY_NAME} from './event_latency_track';
import {ScrollJankV3Track} from './scroll_jank_v3_track';
import {ScrollTimelineTrack} from './scroll_timeline_track';
import {TopLevelScrollTrack} from './scroll_track';
import {ScrollJankCauseMap} from './scroll_jank_cause_map';
import {TrackNode} from '../../public/workspace';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';

export default class implements PerfettoPlugin {
  static readonly id = 'org.chromium.ChromeScrollJank';
  static readonly dependencies = [SqlModulesPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const group = new TrackNode({
      title: 'Chrome Scroll Jank',
      sortOrder: -30,
      isSummary: true,
    });
    await this.addTopLevelScrollTrack(ctx, group);
    await this.addEventLatencyTrack(ctx, group);
    await this.addScrollJankV3ScrollTrack(ctx, group);
    await ScrollJankCauseMap.initialize(ctx.engine);
    await this.addScrollTimelineTrack(ctx, group);
    ctx.workspace.addChildInOrder(group);
    group.expand();
  }

  private async addTopLevelScrollTrack(
    ctx: Trace,
    group: TrackNode,
  ): Promise<void> {
    await ctx.engine.query(`
      INCLUDE PERFETTO MODULE chrome.chrome_scrolls;
      INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_offsets;
      INCLUDE PERFETTO MODULE chrome.event_latency;
    `);

    const uri = 'org.chromium.ChromeScrollJank#toplevelScrolls';
    const title = 'Chrome Scrolls';

    ctx.tracks.registerTrack({
      uri,
      title,
      track: new TopLevelScrollTrack(ctx, uri),
    });

    const track = new TrackNode({uri, title});
    group.addChildInOrder(track);
  }

  private async addEventLatencyTrack(
    ctx: Trace,
    group: TrackNode,
  ): Promise<void> {
    const subTableSql = generateSqlWithInternalLayout({
      columns: ['id', 'ts', 'dur', 'track_id', 'name'],
      sourceTable: 'chrome_event_latencies',
      ts: 'ts',
      dur: 'dur',
      whereClause: `
        event_type IN (
          'FIRST_GESTURE_SCROLL_UPDATE',
          'GESTURE_SCROLL_UPDATE',
          'INERTIAL_GESTURE_SCROLL_UPDATE')
        AND is_presented`,
    });

    // Table name must be unique - it cannot include '-' characters or begin
    // with a numeric value.
    const baseTable = `table_${uuidv4Sql()}_janky_event_latencies_v3`;
    const tableDefSql = `CREATE TABLE ${baseTable} AS
        WITH
        event_latencies AS MATERIALIZED (
          ${subTableSql}
        ),
        latency_stages AS (
          SELECT
            stage.id,
            stage.ts,
            stage.dur,
            stage.track_id,
            stage.name,
            stage.depth,
            event.id as event_latency_id,
            event.depth as event_latency_depth
          FROM event_latencies event
          JOIN descendant_slice(event.id) stage
          UNION ALL
          SELECT
            event.id,
            event.ts,
            event.dur,
            event.track_id,
            IIF(
              id IN (SELECT id FROM chrome_janky_event_latencies_v3),
              '${JANKY_LATENCY_NAME}',
              name
            ) as name,
            0 as depth,
            event.id as event_latency_id,
            event.depth as event_latency_depth
          FROM event_latencies event
        ),
        -- Event latencies have already had layout computed, but the width of event latency can vary (3 or 4),
        -- so we have to compute the max stage depth for each event latency depth to compute offset for each
        -- event latency row.
        event_latency_height_per_row AS (
          SELECT
            event_latency_depth,
            MAX(depth) AS max_depth
          FROM latency_stages
          GROUP BY event_latency_depth
        ),
        -- Compute the offset for each event latency depth using max depth info for each depth.
        event_latency_layout_offset AS (
          SELECT
            event_latency_depth,
            -- As the sum is exclusive, it will return NULL for the first row — we need to set it to 0 explicitly.
            IFNULL(
              SUM(max_depth + 1) OVER (
                ORDER BY event_latency_depth
                ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
              ),
            0) as offset
          FROM event_latency_height_per_row
        )
      SELECT
        stage.id,
        stage.ts,
        stage.dur,
        stage.name,
        stage.depth + (
          (
            SELECT offset.offset
            FROM event_latencies event
            JOIN event_latency_layout_offset offset ON event.depth = offset.event_latency_depth
            WHERE id = stage.event_latency_id
          )
        ) AS depth
      FROM latency_stages stage;`;

    await ctx.engine.query(
      `INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_jank_intervals`,
    );
    await ctx.engine.query(tableDefSql);

    const uri = 'org.chromium.ChromeScrollJank#eventLatency';
    const title = 'Chrome Scroll Input Latencies';

    ctx.tracks.registerTrack({
      uri,
      title,
      track: new EventLatencyTrack(ctx, uri, baseTable),
    });

    const track = new TrackNode({uri, title});
    group.addChildInOrder(track);
  }

  private async addScrollJankV3ScrollTrack(
    ctx: Trace,
    group: TrackNode,
  ): Promise<void> {
    await ctx.engine.query(
      `INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_jank_intervals`,
    );

    const uri = 'org.chromium.ChromeScrollJank#scrollJankV3';
    const title = 'Chrome Scroll Janks';

    ctx.tracks.registerTrack({
      uri,
      title,
      track: new ScrollJankV3Track(ctx, uri),
    });

    const track = new TrackNode({uri, title});
    group.addChildInOrder(track);
  }

  private async addScrollTimelineTrack(
    ctx: Trace,
    group: TrackNode,
  ): Promise<void> {
    const uri = 'org.chromium.ChromeScrollJank#scrollTimeline';
    const title = 'Chrome Scroll Timeline';

    const tableName =
      'scrolltimelinetrack_org_chromium_ChromeScrollJank_scrollTimeline';
    await ScrollTimelineTrack.createTableForTrack(ctx, tableName);

    ctx.tracks.registerTrack({
      uri,
      title,
      track: new ScrollTimelineTrack(ctx, uri, tableName),
    });

    const track = new TrackNode({uri, title});
    group.addChildInOrder(track);
  }
}
