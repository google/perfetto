// Copyright (C) 2024 The Android Open Source Project
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
import {
  NAMED_ROW,
  NamedSliceTrack,
} from '../../components/tracks/named_slice_track';
import {Slice} from '../../public/track';
import {Trace} from '../../public/trace';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventSelection} from '../../public/selection';
import {NUM, STR, STR_NULL} from '../../trace_processor/query_result';
import {escapeQuery} from '../../trace_processor/query_utils';
import {Engine} from '../../trace_processor/engine';
import {rows} from './utils';
import {ColorScheme} from '../../base/color_scheme';
import {JANK_COLOR} from './jank_colors';
import {makeColorScheme} from '../../components/colorizer';
import {HSLColor} from '../../base/color';
import {ScrollTimelineDetailsPanel} from './scroll_timeline_details_panel';

interface StepTemplate {
  // The name of a stage of a scroll.
  // WARNING: This could be an arbitrary string so it MUST BE ESCAPED before
  // using in an SQL query.
  stepName: string;
  // The name of the column in `chrome_scroll_update_info` which contains the
  // timestamp of the step. If not null, this is guaranteed to be a valid column
  // name, i.e. it's safe to use inline in an SQL query without any additional
  // sanitization.
  tsColumnName: string | null;
  // The name of the column in `chrome_scroll_update_info` which contains the
  // duration of the step. Null if the stage doesn't have a duration. If not
  // null, this is guaranteed to be a valid column name, i.e. it's safe to use
  // inline in an SQL query without any additional sanitization.
  durColumnName: string | null;
}

/**
 * Classification of a scroll update for the purposes of trace visualization.
 *
 * If a scroll update matches multiple classifications (e.g. janky and
 * inertial), it should be classified with the highest-priority one (e.g.
 * janky). With the exception of `DEFAULT` and `STEP`, the values are sorted in
 * the order of descending priority (i.e. `JANKY` has the highest priority).
 */
enum ScrollUpdateClassification {
  // None of the other classifications apply.
  DEFAULT = 0,

  // The corresponding frame was janky.
  // See `chrome_scroll_update_input_info.is_janky`.
  JANKY = 1,

  // The input was coalesced into an earlier input's frame.
  // See `chrome_scroll_update_input_info.is_first_scroll_update_in_frame`.
  COALESCED = 2,

  // It's the first scroll update in a scroll.
  // Note: A first scroll update can never be janky.
  // See `chrome_scroll_update_input_info.is_first_scroll_update_in_scroll`.
  FIRST_SCROLL_UPDATE_IN_FRAME = 3,

  // The corresponding scroll was inertial (i.e. a fling).
  INERTIAL = 4,

  // Sentinel value for slices which represent sub-steps of a scroll update.
  STEP = -1,
}

const INDIGO = makeColorScheme(new HSLColor([231, 48, 48]));
const GRAY = makeColorScheme(new HSLColor([0, 0, 62]));
const DARK_GREEN = makeColorScheme(new HSLColor([120, 44, 34]));
const TEAL = makeColorScheme(new HSLColor([187, 90, 42]));

function toColorScheme(
  classification: ScrollUpdateClassification,
): ColorScheme | undefined {
  switch (classification) {
    case ScrollUpdateClassification.DEFAULT:
      return INDIGO;
    case ScrollUpdateClassification.JANKY:
      return JANK_COLOR;
    case ScrollUpdateClassification.COALESCED:
      return GRAY;
    case ScrollUpdateClassification.FIRST_SCROLL_UPDATE_IN_FRAME:
      return DARK_GREEN;
    case ScrollUpdateClassification.INERTIAL:
      return TEAL;
    case ScrollUpdateClassification.STEP:
      return undefined;
  }
}

/**
 * If `allowedColumnNames` contains `columnName`, returns `columnName`.
 * Otherwise, returns null.
 */
function checkColumnNameIsValidOrReturnNull(
  columnName: string | null,
  allowedColumnNames: Set<string>,
  errorMessagePrefix: string,
): string | null {
  if (columnName == null || allowedColumnNames.has(columnName)) {
    return columnName;
  } else {
    console.error(
      `${errorMessagePrefix}: ${columnName}
      (allowed column names: ${Array.from(allowedColumnNames).join(', ')})`,
    );
    return null;
  }
}

const SCROLL_TIMELINE_TRACK_ROW = {
  ...NAMED_ROW,
  classification: NUM,
};
type ScrollTimelineTrackRow = typeof SCROLL_TIMELINE_TRACK_ROW;

export class ScrollTimelineTrack extends NamedSliceTrack<
  Slice,
  ScrollTimelineTrackRow
> {
  /**
   * Constructs a scroll timeline track for a given `trace`.
   *
   * @param trace - The trace whose data the track will display
   * @param uri - The URI of the track
   * @param tableName - The name of an existing SQL table which contains
   * information about the slices of the track. IMPORTANT: You must create
   * the table first using {@link ScrollTimelineTrack.createTableForTrack}
   * BEFORE creating this track.
   */
  constructor(
    trace: Trace,
    uri: string,
    private readonly tableName: string,
  ) {
    super(trace, uri);
  }

  override getSqlSource(): string {
    return `SELECT * FROM ${this.tableName}`;
  }

  override getRowSpec(): ScrollTimelineTrackRow {
    return SCROLL_TIMELINE_TRACK_ROW;
  }

  override rowToSlice(row: ScrollTimelineTrackRow): Slice {
    const baseSlice = super.rowToSliceBase(row);
    const colorScheme = toColorScheme(row.classification);
    if (colorScheme === undefined) {
      return baseSlice;
    } else {
      return {...baseSlice, colorScheme};
    }
  }

  override detailsPanel(sel: TrackEventSelection): TrackEventDetailsPanel {
    return new ScrollTimelineDetailsPanel(
      this.trace,
      this.tableName,
      this.uri,
      sel.eventId,
    );
  }

  /**
   * Creates a Perfetto table named `tableName` representing the slices of a
   * {@link ScrollTimelineTrack} for a given `trace`. You can use this table to
   * construct the track.
   */
  static async createTableForTrack(
    trace: Trace,
    tableName: string,
  ): Promise<void> {
    const engine = trace.engine;
    const stepTemplates = await ScrollTimelineTrack.queryStepTemplates(engine);
    // TODO: b/383549233 - Set ts+dur of each scroll update directly based on
    // our knowledge of the scrolling pipeline (as opposed to aggregating over
    // scroll_steps).
    await engine.query(
      `INCLUDE PERFETTO MODULE chrome.chrome_scrolls;
      CREATE PERFETTO TABLE ${tableName} AS
      WITH
        -- Unpivot all ts+dur columns into rows. Each row corresponds to a step
        -- of a particular scroll update. Some of the rows might have null
        -- ts/dur values, which will be filtered out in unordered_slices.
        -- |scroll_steps| = |chrome_scroll_update_info| * |stepTemplates|
        scroll_steps AS (${stepTemplates
          .map(
            (step) => `
              SELECT
                id AS scroll_update_id,
                ${step.tsColumnName ?? 'NULL'} AS ts,
                ${step.durColumnName ?? 'NULL'} AS dur,
                ${escapeQuery(step.stepName)} AS name
              FROM chrome_scroll_update_info`,
          )
          .join(' UNION ALL ')}),
        -- For each scroll update, find its ts+dur by aggregating over all steps
        -- within the scroll update. We're basically trying to find MIN(COL1_ts,
        -- COL2_ts, ..., COLn_ts) and MAX(COL1_ts, COL2_ts, ..., COLn_ts) from
        -- all the various ts columns in chrome_scroll_update_info. The
        -- difficulty is that some of those columns might be null, which is
        -- better handled by the aggregate MIN/MAX functions (which ignore null
        -- values) than the scalar MIN/MAX functions (which return null if any
        -- argument is null). That's why we do it in such a roundabout way by
        -- joining the top-level table with the individual steps.
        scroll_updates_with_bounds AS (
          SELECT
            scroll_update.id AS scroll_update_id,
            MIN(scroll_steps.ts) AS ts,
            MAX(scroll_steps.ts) - MIN(scroll_steps.ts) AS dur,
            -- Combine all applicable scroll update classifications into the
            -- name. For example, if a scroll update is both janky and inertial,
            -- its name will be name 'Janky Inertial Scroll Update'.
            CONCAT_WS(
              ' ',
              IIF(scroll_update.is_janky, 'Janky', NULL),
              IIF(scroll_update.is_first_scroll_update_in_scroll, 'First', NULL),
              IIF(
                NOT scroll_update.is_first_scroll_update_in_frame,
                'Coalesced',
                NULL
              ),
              IIF(scroll_update.is_inertial, 'Inertial', NULL),
              'Scroll Update'
            ) AS name,
            -- Pick the highest-priority applicable scroll update
            -- classification. For example, if a scroll update is both janky and
            -- inertial, classify it as janky.
            CASE
              WHEN scroll_update.is_janky
                THEN ${ScrollUpdateClassification.JANKY}
              WHEN scroll_update.is_first_scroll_update_in_scroll
                THEN ${ScrollUpdateClassification.FIRST_SCROLL_UPDATE_IN_FRAME}
              WHEN NOT scroll_update.is_first_scroll_update_in_frame
                THEN ${ScrollUpdateClassification.COALESCED}
              WHEN scroll_update.is_inertial
                THEN ${ScrollUpdateClassification.INERTIAL}
              ELSE ${ScrollUpdateClassification.DEFAULT}
            END AS classification
          FROM
            chrome_scroll_update_info AS scroll_update
            JOIN scroll_steps ON scroll_steps.scroll_update_id = scroll_update.id
          GROUP BY scroll_update.id
        ),
        -- Now that we know the ts+dur of all scroll updates, we can lay them
        -- out efficiently (i.e. assign depths to them to avoid overlaps).
        scroll_updates_with_layouts AS (
          ${generateSqlWithInternalLayout({
            columns: [
              'scroll_update_id',
              'ts',
              'dur',
              'name',
              'classification',
            ],
            sourceTable: 'scroll_updates_with_bounds',
            ts: 'ts',
            dur: 'dur',
            // Filter out scroll updates with no timestamps. See b/388756942.
            whereClause: 'ts IS NOT NULL AND dur IS NOT NULL',
          })}
        ),
        -- We interleave the top-level scroll update slices (at even depths) and
        -- their constituent step slices (at odd depths).
        unordered_slices AS (
          SELECT
            ts,
            dur,
            2 * depth AS depth,
            name,
            classification,
            scroll_update_id
          FROM scroll_updates_with_layouts
          UNION ALL
          SELECT
            scroll_steps.ts,
            MAX(scroll_steps.dur, 0) AS dur,
            2 * scroll_updates_with_layouts.depth + 1 AS depth,
            scroll_steps.name,
            ${ScrollUpdateClassification.STEP} AS classification,
            scroll_updates_with_layouts.scroll_update_id
          FROM scroll_steps
          JOIN scroll_updates_with_layouts USING(scroll_update_id)
          WHERE scroll_steps.ts IS NOT NULL AND scroll_steps.dur IS NOT NULL
        )
      -- Finally, we sort all slices chronologically and assign them
      -- monotonically increasing IDs. Note that we cannot reuse
      -- chrome_scroll_update_info.id (not even for the top-level scroll update
      -- slices) because Perfetto slice IDs must be 32-bit unsigned integers.
      SELECT
        ROW_NUMBER() OVER (ORDER BY ts ASC) AS id,
        *
      FROM unordered_slices
      ORDER BY ts ASC`,
    );
  }

  /**
   * Queries scroll step templates from
   * `chrome_scroll_update_info_step_templates`.
   *
   * This function sanitizes the column names `StepTemplate.ts_column_name` and
   * `StepTemplate.dur_column_name`. Unless null, the returned column names are
   * guaranteed to be valid column names of `chrome_scroll_update_info`.
   */
  private static async queryStepTemplates(
    engine: Engine,
  ): Promise<StepTemplate[]> {
    // Use a set for faster lookups.
    const columnNames = new Set(
      await ScrollTimelineTrack.queryChromeScrollUpdateInfoColumnNames(engine),
    );
    const stepTemplatesResult = await engine.query(`
      INCLUDE PERFETTO MODULE chrome.chrome_scrolls;
      SELECT
        step_name,
        ts_column_name,
        dur_column_name
      FROM chrome_scroll_update_info_step_templates;`);
    return rows(stepTemplatesResult, {
      step_name: STR,
      ts_column_name: STR_NULL,
      dur_column_name: STR_NULL,
    }).map(
      // We defensively verify that the column names actually exist in the
      // `chrome_scroll_update_info` table. We do this because we cannot update
      // the `chrome_scroll_update_info` table and this plugin atomically
      // (`chrome_scroll_update_info` is a part of the Chrome tracing stdlib,
      // whose source of truth is in the Chromium repository).
      (row) => ({
        stepName: row.step_name,
        tsColumnName: checkColumnNameIsValidOrReturnNull(
          row.ts_column_name,
          columnNames,
          'Invalid ts_column_name in chrome_scroll_update_info_step_templates',
        ),
        durColumnName: checkColumnNameIsValidOrReturnNull(
          row.dur_column_name,
          columnNames,
          'Invalid dur_column_name in chrome_scroll_update_info_step_templates',
        ),
      }),
    );
  }

  /** Returns the names of columns of the `chrome_scroll_update_info` table. */
  private static async queryChromeScrollUpdateInfoColumnNames(
    engine: Engine,
  ): Promise<string[]> {
    // See https://www.sqlite.org/pragma.html#pragfunc and
    // https://www.sqlite.org/pragma.html#pragma_table_info for more information
    // about `pragma_table_info`.
    const columnNamesResult = await engine.query(`
      INCLUDE PERFETTO MODULE chrome.chrome_scrolls;
      SELECT name FROM pragma_table_info('chrome_scroll_update_info');`);
    return rows(columnNamesResult, {name: STR}).map((row) => row.name);
  }
}
