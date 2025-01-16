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
  NamedRow,
  NamedSliceTrack,
} from '../../components/tracks/named_slice_track';
import {Slice} from '../../public/track';
import {SqlTableSliceTrackDetailsPanel} from '../../components/tracks/sql_table_slice_track_details_tab';
import {Trace} from '../../public/trace';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventSelection} from '../../public/selection';
import {
  QueryResult,
  Row,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {escapeQuery} from '../../trace_processor/query_utils';
import {Engine} from '../../trace_processor/engine';

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

/** Returns an array of the rows in `queryResult`. */
function rows<R extends Row>(queryResult: QueryResult, spec: R): R[] {
  const results: R[] = [];
  for (const it = queryResult.iter(spec); it.valid(); it.next()) {
    const row: Row = {};
    for (const key of Object.keys(spec)) {
      row[key] = it[key];
    }
    results.push(row as R);
  }
  return results;
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

export class ScrollTimelineTrack extends NamedSliceTrack<Slice, NamedRow> {
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

  override getRowSpec(): NamedRow {
    return NAMED_ROW;
  }

  override rowToSlice(row: NamedRow): Slice {
    return super.rowToSliceBase(row);
  }

  override detailsPanel(sel: TrackEventSelection): TrackEventDetailsPanel {
    return new SqlTableSliceTrackDetailsPanel(
      this.trace,
      this.tableName,
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
                id AS scroll_id,
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
        scroll_update_bounds AS (
          SELECT
            scroll_update.id AS scroll_id,
            MIN(scroll_steps.ts) AS ts,
            MAX(scroll_steps.ts) - MIN(scroll_steps.ts) AS dur
          FROM
            chrome_scroll_update_info AS scroll_update
            JOIN scroll_steps ON scroll_steps.scroll_id = scroll_update.id
          GROUP BY scroll_update.id
        ),
        -- Now that we know the ts+dur of all scroll updates, we can lay them
        -- out efficiently (i.e. assign depths to them to avoid overlaps).
        scroll_update_layouts AS (
          ${generateSqlWithInternalLayout({
            columns: ['scroll_id', 'ts', 'dur'],
            sourceTable: 'scroll_update_bounds',
            ts: 'ts',
            dur: 'dur',
          })}
        ),
        -- We interleave the top-level scroll update slices (at even depths) and
        -- their constituent step slices (at odd depths).
        unordered_slices AS (
          SELECT
            ts,
            dur,
            2 * depth AS depth,
            'Scroll Update' AS name
          FROM scroll_update_layouts
          UNION ALL
          SELECT
            scroll_steps.ts,
            MAX(scroll_steps.dur, 0) AS dur,
            2 * scroll_update_layouts.depth + 1 AS depth,
            scroll_steps.name
          FROM scroll_steps
          JOIN scroll_update_layouts USING(scroll_id)
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
