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

import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {
  createPerfettoTable,
  sqlValueToReadableString,
  sqlValueToSqliteString,
} from '../../trace_processor/sql_utils';
import {DatasetSliceTrack} from './dataset_slice_track';
import {
  ARG_PREFIX,
  DebugSliceTrackDetailsPanel,
} from './debug_slice_track_details_panel';
import {
  CounterColumnMapping,
  SqlTableCounterTrack,
} from './query_counter_track';
import {SliceColumnMapping, SqlDataSource} from './query_slice_track';

let trackCounter = 0; // For reproducible ids.

function getUniqueTrackCounter() {
  return trackCounter++;
}

export interface DebugSliceTrackArgs {
  readonly trace: Trace;
  readonly data: SqlDataSource;
  readonly title?: string;
  readonly columns?: Partial<SliceColumnMapping>;
  readonly argColumns?: string[];
  readonly pivotOn?: string;
}

/**
 * Adds a new debug slice track to the workspace.
 *
 * A debug slice track is a track based on a query which is:
 * - Based on a query.
 * - Uses automatic slice layout.
 * - Automatically added to the top of the current workspace.
 * - Pinned.
 * - Has a close button to remove it.
 *
 * @param args - Args to pass to the trace.
 * @param args.trace - The trace to use.
 * @param args.data.sqlSource - The query to run.
 * @param args.data.columns - Optional: Override columns.
 * @param args.title - Optional: Title for the track. If pivotOn is supplied,
 * this will be used as the root title for each track, but each title will have
 * the value appended.
 * @param args.columns - Optional: The columns names to use for the various
 * essential column names.
 * @param args.argColumns - Optional: A list of columns which are passed to the
 * details panel.
 * @param args.pivotOn - Optional: The name of a column on which to pivot. If
 * provided, we will create N tracks, one for each distinct value of the pivotOn
 * column. Each track will only show the slices which have the corresponding
 * value in their pivotOn column.
 */
export async function addDebugSliceTrack(args: DebugSliceTrackArgs) {
  const tableId = getUniqueTrackCounter();
  const tableName = `__debug_track_${tableId}`;
  const titleBase = args.title?.trim() || `Debug Slice Track ${tableId}`;
  const uriBase = `debug.track${tableId}`;

  // Create a table for this query before doing anything
  await createTableForSliceTrack(
    args.trace.engine,
    tableName,
    args.data,
    args.columns,
    args.argColumns,
    args.pivotOn,
  );

  if (args.pivotOn) {
    await addPivotedSliceTracks(
      args.trace,
      tableName,
      titleBase,
      uriBase,
      args.pivotOn,
    );
  } else {
    addSingleSliceTrack(args.trace, tableName, titleBase, uriBase);
  }
}

async function createTableForSliceTrack(
  engine: Engine,
  tableName: string,
  data: SqlDataSource,
  columns: Partial<SliceColumnMapping> = {},
  argColumns?: string[],
  pivotCol?: string,
) {
  const {ts = 'ts', dur = 'dur', name = 'name'} = columns;

  // If the view has clashing names (e.g. "name" coming from joining two
  // different tables, we will see names like "name_1", "name_2", but they
  // won't be addressable from the SQL. So we explicitly name them through a
  // list of columns passed to CTE.
  const dataColumns =
    data.columns !== undefined ? `(${data.columns.join(', ')})` : '';

  const cols = [
    `${ts} as ts`,
    `ifnull(cast(${dur} as int), -1) as dur`,
    `printf('%s', ${name}) as name`,
    argColumns && argColumns.map((c) => `${c} as ${ARG_PREFIX}${c}`),
    pivotCol && `${pivotCol} as pivot`,
  ]
    .flat() // Convert to flattened list
    .filter(Boolean) // Remove falsy values
    .join(',');

  const query = `
    with data${dataColumns} as (
      ${data.sqlSource}
    ),
    prepared_data as (
      select ${cols}
      from data
    )
    select
      row_number() over (order by ts) as id,
      *
    from prepared_data
    order by ts
  `;

  return await createPerfettoTable(engine, tableName, query);
}

async function addPivotedSliceTracks(
  trace: Trace,
  tableName: string,
  titleBase: string,
  uriBase: string,
  pivotColName: string,
) {
  const result = await trace.engine.query(`
    SELECT DISTINCT pivot
    FROM ${tableName}
    ORDER BY pivot
  `);

  let trackCount = 0;
  for (const iter = result.iter({}); iter.valid(); iter.next()) {
    const uri = `${uriBase}_${trackCount++}`;
    const pivotValue = iter.get('pivot');
    const title = `${titleBase}: ${pivotColName} = ${sqlValueToReadableString(pivotValue)}`;

    trace.tracks.registerTrack({
      uri,
      title,
      track: new DatasetSliceTrack({
        trace,
        uri,
        dataset: new SourceDataset({
          schema: {
            id: NUM,
            ts: LONG,
            dur: LONG,
            name: STR,
          },
          src: tableName,
          filter: {
            col: 'pivot',
            eq: pivotValue,
          },
        }),
        detailsPanel: (row) => {
          return new DebugSliceTrackDetailsPanel(trace, tableName, row.id);
        },
      }),
    });

    const trackNode = new TrackNode({uri, title, removable: true});
    trace.workspace.pinnedTracksNode.addChildLast(trackNode);
  }
}

function addSingleSliceTrack(
  trace: Trace,
  tableName: string,
  title: string,
  uri: string,
) {
  trace.tracks.registerTrack({
    uri,
    title,
    track: new DatasetSliceTrack({
      trace,
      uri,
      dataset: new SourceDataset({
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          name: STR,
        },
        src: tableName,
      }),
      detailsPanel: (row) => {
        return new DebugSliceTrackDetailsPanel(trace, tableName, row.id);
      },
    }),
  });

  const trackNode = new TrackNode({uri, title, removable: true});
  trace.workspace.pinnedTracksNode.addChildLast(trackNode);
}

export interface DebugCounterTrackArgs {
  readonly trace: Trace;
  readonly data: SqlDataSource;
  readonly title?: string;
  readonly columns?: Partial<CounterColumnMapping>;
  readonly pivotOn?: string;
}

/**
 * Adds a new debug counter track to the workspace.
 *
 * A debug slice track is a track based on a query which is:
 * - Based on a query.
 * - Automatically added to the top of the current workspace.
 * - Pinned.
 * - Has a close button to remove it.
 *
 * @param args - Args to pass to the trace.
 * @param args.trace - The trace to use.
 * @param args.data.sqlSource - The query to run.
 * @param args.data.columns - Optional: Override columns.
 * @param args.title - Optional: Title for the track. If pivotOn is supplied,
 * this will be used as the root title for each track, but each title will have
 * the value appended.
 * @param args.columns - Optional: The columns names to use for the various
 * essential column names.
 * @param args.pivotOn - Optional: The name of a column on which to pivot. If
 * provided, we will create N tracks, one for each distinct value of the pivotOn
 * column. Each track will only show the slices which have the corresponding
 * value in their pivotOn column.
 */
export async function addDebugCounterTrack(args: DebugCounterTrackArgs) {
  const tableId = getUniqueTrackCounter();
  const tableName = `__debug_track_${tableId}`;
  const titleBase = args.title?.trim() || `Debug Slice Track ${tableId}`;
  const uriBase = `debug.track${tableId}`;

  // Create a table for this query before doing anything
  await createTableForCounterTrack(
    args.trace.engine,
    tableName,
    args.data,
    args.columns,
    args.pivotOn,
  );

  if (args.pivotOn) {
    await addPivotedCounterTracks(
      args.trace,
      tableName,
      titleBase,
      uriBase,
      args.pivotOn,
    );
  } else {
    addSingleCounterTrack(args.trace, tableName, titleBase, uriBase);
  }
}

async function createTableForCounterTrack(
  engine: Engine,
  tableName: string,
  data: SqlDataSource,
  columnMapping: Partial<CounterColumnMapping> = {},
  pivotCol?: string,
) {
  const {ts = 'ts', value = 'value'} = columnMapping;
  const cols = [
    `${ts} as ts`,
    `${value} as value`,
    pivotCol && `${pivotCol} as pivot`,
  ]
    .flat() // Convert to flattened list
    .filter(Boolean) // Remove falsy values
    .join(',');

  const query = `
    with data as (
      ${data.sqlSource}
    )
    select ${cols}
    from data
    order by ts
  `;

  return await createPerfettoTable(engine, tableName, query);
}

async function addPivotedCounterTracks(
  trace: Trace,
  tableName: string,
  titleBase: string,
  uriBase: string,
  pivotColName: string,
) {
  const result = await trace.engine.query(`
    SELECT DISTINCT pivot
    FROM ${tableName}
    ORDER BY pivot
  `);

  let trackCount = 0;
  for (const iter = result.iter({}); iter.valid(); iter.next()) {
    const uri = `${uriBase}_${trackCount++}`;
    const pivotValue = iter.get('pivot');
    const title = `${titleBase}: ${pivotColName} = ${sqlValueToReadableString(pivotValue)}`;

    trace.tracks.registerTrack({
      uri,
      title,
      track: new SqlTableCounterTrack(
        trace,
        uri,
        `
          SELECT *
          FROM ${tableName}
          WHERE pivot = ${sqlValueToSqliteString(pivotValue)}
        `,
      ),
    });

    const trackNode = new TrackNode({uri, title, removable: true});
    trace.workspace.pinnedTracksNode.addChildLast(trackNode);
  }
}

function addSingleCounterTrack(
  trace: Trace,
  tableName: string,
  title: string,
  uri: string,
) {
  trace.tracks.registerTrack({
    uri,
    title,
    track: new SqlTableCounterTrack(trace, uri, tableName),
  });

  const trackNode = new TrackNode({uri, title, removable: true});
  trace.workspace.pinnedTracksNode.addChildLast(trackNode);
}
