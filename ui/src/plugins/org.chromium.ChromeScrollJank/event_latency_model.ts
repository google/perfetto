// Copyright (C) 2026 The Android Open Source Project
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

import {generateSqlWithInternalLayout} from '../../components/sql_utils/layout';
import {Engine} from '../../trace_processor/engine';
import {JANKY_LATENCY_NAME} from './event_latency_track';
import {EVENT_LATENCY_TRACK} from './tracks';

export async function createEventLatencyModel(engine: Engine): Promise<void> {
  await engine.query(`
    INCLUDE PERFETTO MODULE chrome.event_latency;
    INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_jank_intervals;

    CREATE TABLE ${EVENT_LATENCY_TRACK.tableName} AS
      WITH
      event_latencies AS MATERIALIZED (
        ${generateSqlWithInternalLayout({
          columns: ['id', 'ts', 'dur', 'track_id', 'name'],
          source: 'chrome_event_latencies',
          ts: 'ts',
          dur: 'dur',
          whereClause: `
            event_type IN (
              'FIRST_GESTURE_SCROLL_UPDATE',
              'GESTURE_SCROLL_UPDATE',
              'INERTIAL_GESTURE_SCROLL_UPDATE')
            AND is_presented`,
        })}
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
    FROM latency_stages stage;`);
}
