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

/**
 * A model of the scroll timeline according to Chrome's scroll jank v4 metric.
 *
 * See
 * https://docs.google.com/document/d/1AaBvTIf8i-c-WTKkjaL4vyhQMkSdynxo3XEiwpofdeA
 * and scroll_jank_v4*.{h,cc} source files in
 * https://source.chromium.org/chromium/chromium/src/+/main:cc/metrics/ for more
 * details about the v4 metric.
 */
export interface ScrollTimelineV4Model {
  /**
   * The name of the SQL table which contains information about the slices in
   * the track visualizing the timeline created by
   * {@link scroll_timeline_v4_track#createScrollTimelineV4Track}. Each slice
   * corresponds to a single frame that contains one or more scroll updates.
   *
   * The table has the following columns:
   *
   *   id (NUM): Unique ID of the slice (monotonically increasing). Note that
   *     it cannot joined with any tables in Chrome's tracing stdlib. Not
   *     guaranteed to be stable.
   *   ts (LONG/TIMESTAMP): Start timestamp of the slice.
   *   dur (LONG/DURATION): Duration of the slice.
   *   depth (NUM): Depth of the slice on the track.
   *   name (STRING): Title of the slice.
   *   classification (NUM): Classification of the frame for the purposes of
   *     trace visualization. Guaranteed to be one of the values in
   *     {@link ScrollFrameClassification}.
   */
  readonly tableName: string;

  /** A unique identifier of the track. */
  readonly trackUri: string;
}

/**
 * Classification of a frame that contains one or more scroll updates, for the
 * purposes of trace visualization.
 *
 * If a frame matches multiple classifications (e.g. janky and inertial), it
 * should be classified with the highest-priority one (e.g. janky). With the
 * exception of `DEFAULT` and `DESCENDANT_SLICE`, the values are sorted in
 * the order of descending priority (i.e. `JANKY` has the highest priority).
 *
 * The classifications correspond to values in `chrome_scroll_jank_v4_results`.
 */
export enum ScrollFrameClassification {
  // None of the other classifications apply.
  DEFAULT = 0,

  // The frame is janky.
  JANKY = 1,

  // The frame is non-damaging.
  NON_DAMAGING = 2,

  // The frame only contains synthetic scroll updates.
  SYNTHETIC = 3,

  // The frame is the first one in a scroll.
  //
  // Note: A first scroll update can never be janky.
  FIRST_FRAME_IN_SCROLL = 4,

  // The frame contained inertial scroll updates (i.e. a fling).
  INERTIAL = 5,

  // Sentinel value reserved for descendant slices.
  DESCENDANT_SLICE = -1,
}

export async function createScrollTimelineV4Model(
  engine: Engine,
  tableName: string,
  trackUri: string,
): Promise<ScrollTimelineV4Model> {
  await createTable(engine, tableName);
  return {tableName, trackUri};
}

/**
 * Creates a Perfetto table named `tableName` representing the slices of a the
 * track created by {@link scroll_timeline_v4_track#createScrollTimelineV4Track}
 * for a given trace.
 */
async function createTable(engine: Engine, tableName: string): Promise<void> {
  await engine.query(
    `INCLUDE PERFETTO MODULE chrome.scroll_jank_v4;

    CREATE PERFETTO TABLE ${tableName} AS
    WITH descendant_slices AS (
      SELECT
        ancestor.id AS ancestor_id,
        descendant.ts,
        descendant.dur,
        descendant.name,
        descendant.depth
      FROM chrome_scroll_jank_v4_results AS ancestor
      JOIN descendant_slice(ancestor.id) AS descendant
    ),
    max_depth AS (
      SELECT
        COALESCE((SELECT MAX(depth) FROM descendant_slices), 0) AS max_depth
    ),
    frame_layout AS (
      ${generateSqlWithInternalLayout({
        columns: ['id'],
        source: 'chrome_scroll_jank_v4_results',
        ts: 'ts',
        dur: 'dur',
      })}
    ),
    timeline_slices_without_id AS (
      SELECT
        results.ts,
        results.dur,
        frame_layout.depth * (max_depth.max_depth + 1) AS depth,
        CONCAT_WS(
          ' ',
          IIF(results.is_janky, 'Janky'),
          IIF(
            results.damage_type LIKE 'NON_DAMAGING%',
            'Non-damaging'
          ),
          IIF(
            results.real_first_input_generation_ts IS NULL
            AND results.synthetic_first_original_begin_frame_ts IS NOT NULL,
            'Synthetic'
          ),
          IIF(results.vsyncs_since_previous_frame IS NULL, 'First'),
          IIF(
            results.real_max_abs_inertial_raw_delta_pixels IS NOT NULL,
            'Inertial'
          ),
          'Frame'
        ) AS name,
        CASE
          WHEN is_janky
            THEN ${ScrollFrameClassification.JANKY}
          WHEN results.damage_type LIKE 'NON_DAMAGING%'
            THEN ${ScrollFrameClassification.NON_DAMAGING}
          WHEN results.real_first_input_generation_ts IS NULL
            AND results.synthetic_first_original_begin_frame_ts IS NOT NULL
            THEN ${ScrollFrameClassification.SYNTHETIC}
          WHEN vsyncs_since_previous_frame IS NULL
            THEN ${ScrollFrameClassification.FIRST_FRAME_IN_SCROLL}
          WHEN real_max_abs_inertial_raw_delta_pixels IS NOT NULL
            THEN ${ScrollFrameClassification.INERTIAL}
          ELSE ${ScrollFrameClassification.DEFAULT}
        END AS classification
      FROM chrome_scroll_jank_v4_results AS results
      JOIN frame_layout USING(id)
      JOIN max_depth
      UNION ALL
      SELECT
        descendant.ts,
        descendant.dur,
        frame_layout.depth * (max_depth.max_depth + 1) + descendant.depth
          AS depth,
        descendant.name,
        ${ScrollFrameClassification.DESCENDANT_SLICE} AS classification
      FROM descendant_slices AS descendant
      JOIN frame_layout ON descendant.ancestor_id = frame_layout.id
      JOIN max_depth
    )
    SELECT
      row_number() OVER (ORDER BY ts ASC) AS id,
      *
    FROM timeline_slices_without_id
    ORDER BY ts ASC;`,
  );
}
