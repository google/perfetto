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

import {okResult, Result} from '../../base/result';
import {generateSqlWithInternalLayout} from '../../components/sql_utils/layout';
import {Engine} from '../../trace_processor/engine';
import {NUM} from '../../trace_processor/query_result';

/**
 * Classification of a slice on the frame timeline track frame created via
 * {@link frame_timeline_track#createFrameTimelineTrack}.
 */
export enum FrameTimelineSliceClassification {
  // A header "Frame timelines" slice. Each header slice corresponds to a single
  // "Extend_VSync" slice in the trace. All other slices on the frame timeline
  // are descendants of a header slice and are derived from the arguments of the
  // corresponding "Extend_VSync" slice.
  //
  // Its start and end timestamps are set to the minimum start and maximum end
  // timestamps of its descendant slices respectively.
  HEADER = 0,

  // An instant slice which corresponds to the frame time provided by Android.
  //
  // Its timestamp is the time at which the frame started being rendered. It's
  // taken from the "android_choreographer_frame_callback_data.frame_time_us"
  // argument of the "Extend_VSync" slice, converted from monotonic clock time
  // to a trace timestamp.
  //
  // See
  // https://developer.android.com/ndk/reference/group/choreographer#achoreographerframecallbackdata_getframetimenanos.
  FRAME_TIME = 1,

  // A copy of the "Extend_VSync" slice for reference.
  EXTEND_VSYNC = 2,

  // A slice which corresponds to a possible frame timeline which Android
  // prefers, i.e. whose index in the
  // "android_choreographer_frame_callback_data.frame_timeline" argument list is
  // equal to the
  // "android_choreographer_frame_callback_data.preferred_frame_timeline_index"
  // argument.
  //
  // The start timestamp is the time at which the frame needs to be ready by in
  // order to be presented on time (aka "latch deadline"). The end timestamp is
  // the time at which the frame is expected to be presented. The timestamps are
  // calculated from the
  // "android_choreographer_frame_callback_data.frame_timeline[index].latch_delta_us"
  // and
  // "android_choreographer_frame_callback_data.frame_timeline[index].present_delta_us"
  // arguments respectively.
  //
  // See
  // https://developer.android.com/ndk/reference/group/choreographer#achoreographerframecallbackdata_getframetimelinedeadlinenanos,
  // https://developer.android.com/ndk/reference/group/choreographer#achoreographerframecallbackdata_getframetimelineexpectedpresentationtimenanos
  // and
  // https://developer.android.com/ndk/reference/group/choreographer#achoreographerframecallbackdata_getpreferredframetimelineindex.
  PREFERRED_TIMELINE = 3,

  // A slice which corresponds to a possible frame timeline which Android
  // DOESN'T prefer, i.e. whose index in the
  // "android_choreographer_frame_callback_data.frame_timeline" argument list is
  // different from the the
  // "android_choreographer_frame_callback_data.preferred_frame_timeline_index"
  // argument. Otherwise the same as `PREFERRED_TIMELINE`.
  NON_PREFERRED_TIMELINE = 4,
}

export interface CreateFrameTimelineModelArgs {
  engine: Engine;

  // The name of the table that `createFrameTimelineModel()` should populate.
  tableName: string;
}

export interface FrameTimelineModel {
  // The name of the table that contains the model created via
  // `createFrameTimelineModel()`.
  tableName: string;

  // Whether the table contains at least one frame timeline.
  hasFrameTimelines: boolean;
}

/**
 * Creates a Perfetto table named `args.tableName` representing the slices of a
 * the track created by {@link frame_timeline_track#createFrameTimelineTrack}
 * for a given trace.
 *
 * Assuming the correct tracing categories are enabled, each time Chrome
 * receives a VSync signal from Android, it emits an "Extend_VSync" trace event
 * with information about the possible frame timelines provided by Android. See
 * https://developer.android.com/ndk/reference/group/choreographer#achoreographerframecallbackdata_getframetimelineslength
 * and `AndroidChoreographerFrameCallbackData` in
 * protos/third_party/chromium/chrome_track_event.proto for more details. For
 * each "Extend_VSync" trace event, this function will populate `args.tableName`
 * with one "Frame timelines" header slice together with several descendant
 * slices, creating the following overall layout:
 *
 * ```
 * +-----------------+  +-----------------+  +-----------------+
 * | Frame timelines |  | Frame timelines |  | Frame timelines |
 * +-----------------+  +-----------------+  +-----------------+
 *  ...descendant...     ...descendant...     ...descendant...
 *    ...slices...         ...slices...         ...slices...
 *        +-----------------+  +-----------------+  +-----------------+
 *        | Frame timelines |  | Frame timelines |  | Frame timelines |
 *        +-----------------+  +-----------------+  +-----------------+
 *         ...descendant...     ...descendant...     ...descendant...
 *           ...slices...         ...slices...         ...slices...
 *               +-----------------+  +-----------------+  +-----------------+
 *               | Frame timelines |  | Frame timelines |  | Frame timelines |
 *               +-----------------+  +-----------------+  +-----------------+
 *                ...descendant...     ...descendant...     ...descendant...
 *                  ...slices...         ...slices...         ...slices...
 * ```
 *
 * There are three types of descendant slices:
 *
 *   1. exactly one "Frame time" instant slice, which corresponds to the frame
 *      time provided by Android,
 *   2. exactly one "Extend_VSync" slice, which is a copy of the original
 *      "Extend_VSync" slice, and
 *   3. one or more frame timelines, which correspond to possible frame
 *      timelines provided by Android, one of which Android marked as preferred.
 *
 * If we zoom in on a single "Frame timelines" slice, its descendants typically
 * look like this:
 *
 * ```
 *  +------------------------------------------------------------------------...
 *  | Frame timelines
 *  +------------------------------------------------------------------------...
 *  ^   +----+    +--------------------+    +--------------------+    +------...
 * /F\  | EV |    | X [0]              |    | X+2 [2]            |    | X+4 [4]
 * |T|  +----+    +--------------------+    +--------------------+    +------...
 *                              +--------------------+    +------------------...
 *                              | X+1 [1, preferred] |    | x+3 [3]
 *                              +--------------------+    +------------------...
 * ```
 *
 * where "FT" is "Frame time", "EV" is "Extend_VSync" and "X" is the token used
 * by Android to identify the frame timeline (some large integer). See
 * https://developer.android.com/ndk/reference/group/choreographer#achoreographerframecallbackdata_getframetimelinevsyncid.
 *
 * Note: With certain Chrome browser tracing configurations, Chrome only records
 * the preferred frame timeline (via
 * "android_choreographer_frame_callback_data.chrome_preferred_frame_timeline.*"
 * arguments), in which case there is only one frame timeline slice named "X
 * [preferred]".
 */
export async function createFrameTimelineModel(
  args: CreateFrameTimelineModelArgs,
): Promise<Result<FrameTimelineModel>> {
  const createTableResult = await args.engine.tryQuery(
    `CREATE PERFETTO TABLE ${args.tableName} AS
    WITH
      -- Find all "Extend_VSync" slices and extract their non-repeated
      -- arguments.
      extend_vsync_slices AS (
        SELECT
          id,
          arg_set_id,
          name,
          ts,
          dur,
          -- Convert "android_choreographer_frame_callback_data.frame_time_us"
          -- from monotonic clock time to a trace timestamp. We assume that
          -- TO_MONOTONIC(A) - TO_MONOTONIC(B) = A - B, substitute B=ts and
          -- TO_MONOTONIC(A)="frame_time_us"*1000 and rearrange to get A.
          ts + extract_arg(
            arg_set_id,
            'android_choreographer_frame_callback_data.frame_time_us'
          ) * 1000 - TO_MONOTONIC(ts) AS frame_ts,
          extract_arg(
            arg_set_id,
            'android_choreographer_frame_callback_data.preferred_frame_timeline_index'
          ) AS preferred_timeline_index
        FROM slice
        WHERE name = 'Extend_VSync'
      ),
      -- Extract the frame timelines in all "Extend_VSync" slices.
      frame_timelines AS (
        -- Find indices N of all
        -- "android_choreographer_frame_callback_data.frame_timeline[N]"
        -- arguments of "Extend_VSync" slices.
        WITH frame_timeline_indices AS (
          SELECT DISTINCT
            extend_vsync_slices.id,
            CAST(
              substr(
                substr(args.key, 1, instr(args.key, ']') - 1),
                instr(args.key, '[') + 1
              )
              AS INTEGER
            ) AS timeline_index
          FROM extend_vsync_slices
          JOIN args USING (arg_set_id)
          WHERE
            -- "[...]" represents a range in a glob pattern, so we must escape
            -- "[" as "[[]" and "]" as "[]]".
            args.key GLOB
              'android_choreographer_frame_callback_data.frame_timeline[[]*[]].*'
        )
        SELECT
          s.id,
          i.timeline_index,
          i.timeline_index = s.preferred_timeline_index AS is_preferred,
          extract_arg(
            s.arg_set_id,
            'android_choreographer_frame_callback_data.frame_timeline[' ||
              i.timeline_index ||
              '].vsync_id'
          ) AS vsync_id,
          -- In case we don't have the frame time, approximate it with the
          -- timestamp of the "Extend_VSync" slice.
          COALESCE(s.frame_ts, s.ts) + extract_arg(
            s.arg_set_id,
            'android_choreographer_frame_callback_data.frame_timeline[' ||
              i.timeline_index ||
              '].latch_delta_us'
          ) * 1000 AS latch_ts,
          COALESCE(s.frame_ts, s.ts) + extract_arg(
            s.arg_set_id,
            'android_choreographer_frame_callback_data.frame_timeline[' ||
              i.timeline_index ||
              '].present_delta_us'
          ) * 1000 AS present_ts
        FROM frame_timeline_indices AS i
        JOIN extend_vsync_slices AS s USING (id)
        UNION ALL
        SELECT
          id,
          NULL AS timeline_index,
          TRUE AS is_preferred,
          extract_arg(
            arg_set_id,
            'android_choreographer_frame_callback_data.chrome_preferred_frame_timeline.vsync_id'
          ) AS vsync_id,
          COALESCE(frame_ts, ts) + extract_arg(
            arg_set_id,
            'android_choreographer_frame_callback_data.chrome_preferred_frame_timeline.latch_delta_us'
          ) * 1000 AS latch_ts,
          COALESCE(frame_ts, ts) + extract_arg(
            arg_set_id,
            'android_choreographer_frame_callback_data.chrome_preferred_frame_timeline.present_delta_us'
          ) * 1000 AS present_ts
        FROM extend_vsync_slices
        WHERE extract_arg(
          arg_set_id,
          'android_choreographer_frame_callback_data.chrome_preferred_frame_timeline.vsync_id'
        ) IS NOT NULL
      ),
      -- For each header slice (partitionByClause), lay out all its descendant
      -- slices (i.e. calculate their depths under the header slice so that
      -- there would be no overlaps).
      descendant_layout AS (
        ${generateSqlWithInternalLayout({
          // This is where we actually define the descendant slices for all
          // header slice. Note that all descendant slices of a particular
          // header slice share the same "id".
          source: `
            SELECT
              id,
              'Frame time' AS name,
              ${FrameTimelineSliceClassification.FRAME_TIME} AS type,
              frame_ts AS ts,
              0 AS dur
            FROM extend_vsync_slices
            WHERE frame_ts IS NOT NULL
            UNION ALL
            SELECT
              id,
              name,
              ${FrameTimelineSliceClassification.EXTEND_VSYNC} AS type,
              ts,
              dur
            FROM extend_vsync_slices
            UNION ALL
            SELECT
              id,
              frame_timelines.vsync_id || ' [' ||
                concat_ws(
                  ', ',
                  frame_timelines.timeline_index,
                  IF(frame_timelines.is_preferred, 'preferred')
                ) || ']'  AS name,
              IF(
                frame_timelines.is_preferred,
                ${FrameTimelineSliceClassification.PREFERRED_TIMELINE},
                ${FrameTimelineSliceClassification.NON_PREFERRED_TIMELINE}
              ) AS type,
              latch_ts AS ts,
              present_ts - latch_ts AS dur
            FROM frame_timelines`,
          ts: 'ts',
          dur: 'dur',
          columns: ['id', 'ts', 'dur', 'name', 'type'],
          partitionByClause: 'id',
        })}
      ),
      -- Lay out the header slices (i.e. calculate their relative depths so that
      -- there would be no overlaps).
      header_layout AS (
        ${generateSqlWithInternalLayout({
          // This is where we actually define the header slices. We must define
          // the header slices AFTER the descendant slices so that we could
          // calculate the header slices' timestamps and durations.
          source: `
            SELECT
              id,
              'Frame timelines' AS name,
              ${FrameTimelineSliceClassification.HEADER} AS type,
              MIN(ts) AS ts,
              MAX(ts + dur) - MIN(ts) AS dur
            FROM descendant_layout
            GROUP BY id`,
          ts: 'ts',
          dur: 'dur',
          columns: ['id', 'ts', 'dur', 'name', 'type'],
        })}
      ),
      -- The header_layout.depth column contains the calculated depths of the
      -- header slices relative to each other, ignoring their descendant slices.
      -- Similarly, descendant_layout.depth contains the calculated depths of
      -- the descendant slices relative to other descendant slices under the
      -- same header slice. We now need to combine them into a single "layered"
      -- layout which looks like this:
      --
      --   H0. header slices with header_layout.depth=0
      --   D0. descendant slices (any descendant_layout.depth) whose header
      --       slice has header_layout.depth=0
      --   H1. header slices with header_layout.depth=1
      --   D1. descendant slices (any descendant_layout.depth) whose header
      --       slice has header_layout.depth=1
      --   ...
      --
      -- Each header layers (H0, H1, ...) occupies exactly one depth level. In
      -- contrast, descendant layers (D0, D1, ...) occupy a variable number of
      -- depth levels, depending on the maximum descendant depth within each
      -- layer.
      max_descendant_levels AS (
        SELECT
          header_layout.depth AS layer,
          COALESCE(MAX(descendant_layout.depth + 1), 0) AS max_descendant_levels
        FROM header_layout
        LEFT JOIN descendant_layout USING(id)
        GROUP BY header_layout.depth
      ),
      -- We then calculate the final depth of the headers as the sum of the
      -- number of levels occupied by the preceding layers. The final depth of
      -- header slices in layer HN is [levels(H0) + levels(D0)] + [levels(H1) +
      -- levels(D1)] + ... + [levels(H(N-1)) + levels(D(N-1))] = [1 +
      -- max_descendant_levels(layer=0)] + [1 + max_descendant_levels(layer=1)]
      -- + ... + [1 + max_descendant_levels(layer=N-1)]. Note that final depth
      -- of header slices in layer H0 is 0.
      header_layout_with_final_depths AS (
        SELECT
          header_layout.id,
          header_layout.name,
          header_layout.type,
          header_layout.ts,
          header_layout.dur,
          final_header_depths.depth
        FROM header_layout
        JOIN (
          SELECT
            layer,
            -- Use TOTAL (rather than SUM) to get depth=0 (rather than NULL) for
            -- layer=0, which has no preceding rows.
            TOTAL(1 + max_descendant_levels) OVER (
              ORDER BY layer ASC
              ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
            ) AS depth
          FROM max_descendant_levels
        ) AS final_header_depths
          ON header_layout.depth = final_header_depths.layer
      ),
      -- We can now stitch it all together, interleaving header and descendant
      -- layers.
      slices_without_ids AS (
        -- Header slices.
        SELECT
          name,
          type,
          ts,
          dur,
          depth
        FROM header_layout_with_final_depths
        UNION ALL
        -- Descendant slices.
        SELECT
          descendant_layout.name,
          descendant_layout.type,
          descendant_layout.ts,
          descendant_layout.dur,
          -- depth=header_layout_with_final_depths.depth contains header slices.
          -- We add 1 so that descendant slices with descendant_layout.depth=0
          -- would end up at depth=header_layout_with_final_depths.depth+1.
          header_layout_with_final_depths.depth + descendant_layout.depth + 1
            AS depth
        FROM descendant_layout
        JOIN header_layout_with_final_depths USING (id)
      )
    -- Finally, assign a unique ID to each slice. Note that we cannot reuse
    -- header_layout_with_final_depths.id or descendant_layout.id because it's
    -- shared between each header and all its descendants.
    SELECT
      ROW_NUMBER() OVER (ORDER BY ts ASC) AS id,
      *
    FROM slices_without_ids;`,
  );
  if (!createTableResult.ok) {
    return createTableResult;
  }

  const queryHasFrameTimelinesResult = await args.engine.tryQuery(`
    SELECT
      EXISTS(
        SELECT 1
        FROM ${args.tableName}
        WHERE type IN (
          ${FrameTimelineSliceClassification.PREFERRED_TIMELINE},
          ${FrameTimelineSliceClassification.NON_PREFERRED_TIMELINE}
        )
      ) AS has_frame_timelines`);
  if (!queryHasFrameTimelinesResult.ok) {
    return queryHasFrameTimelinesResult;
  }

  return okResult({
    tableName: args.tableName,
    hasFrameTimelines:
      queryHasFrameTimelinesResult.value.maybeFirstRow({
        has_frame_timelines: NUM,
      })?.has_frame_timelines !== 0,
  });
}
