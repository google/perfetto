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
import {LONG, NUM, STR, UNKNOWN} from '../../trace_processor/query_result';
import {
  createPerfettoTable,
  sqlValueToReadableString,
  sqlValueToSqliteString,
} from '../../trace_processor/sql_utils';
import {SliceTrack} from './slice_track';
import {
  RAW_PREFIX,
  DebugSliceTrackDetailsPanel,
} from './debug_slice_track_details_panel';
import {
  CounterColumnMapping,
  SqlTableCounterTrack,
} from './query_counter_track';
import {getColorForSlice} from '../colorizer';

export interface SqlDataSource {
  // SQL source selecting the necessary data.
  readonly sqlSource: string;

  // Optional: Rename columns from the query result.
  // If omitted, original column names from the query are used instead.
  // The caller is responsible for ensuring that the number of items in this
  // list matches the number of columns returned by sqlSource.
  readonly columns?: ReadonlyArray<string>;
}

export interface SliceColumnMapping {
  readonly ts: string;
  readonly dur: string;
  readonly name: string;
}

let trackCounter = 0; // For reproducible ids.

function getUniqueTrackCounter() {
  return trackCounter++;
}

export interface DebugSliceTrackArgs {
  readonly trace: Trace;
  readonly data: SqlDataSource;
  readonly title?: string;
  readonly columns?: Partial<SliceColumnMapping>;
  readonly rawColumns?: ReadonlyArray<string>;
  readonly pivotOn?: string;
  readonly argSetIdColumn?: string;
  readonly colorColumn?: string;
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
 * @param args.rawColumns - Optional: A list of columns to be displayed in the
 * 'Raw columns' section of the details panel.
 * @param args.pivotOn - Optional: The name of a column on which to pivot. If
 * provided, we will create N tracks, one for each distinct value of the pivotOn
 * column. Each track will only show the slices which have the corresponding
 * value in their pivotOn column.
 * @param args.colorColumn - Optional: The name of a column to use for coloring
 * slices. If provided, slices will be colored based on the value in this column.
 * If omitted, slices are colored based on their name.
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
    args.rawColumns,
    args.pivotOn,
    args.argSetIdColumn,
    args.colorColumn,
  );

  if (args.pivotOn) {
    await addPivotedSliceTracks(
      args.trace,
      tableName,
      titleBase,
      uriBase,
      args.pivotOn,
      args.colorColumn,
    );
  } else {
    addSingleSliceTrack(
      args.trace,
      tableName,
      titleBase,
      uriBase,
      args.argSetIdColumn,
      args.colorColumn,
    );
  }
}

async function createTableForSliceTrack(
  engine: Engine,
  tableName: string,
  data: SqlDataSource,
  columns: Partial<SliceColumnMapping> = {},
  rawColumns?: ReadonlyArray<string>,
  pivotCol?: string,
  argSetIdColumn?: string,
  colorCol?: string,
) {
  if (rawColumns === undefined) {
    // Find the raw columns list from the query if not provided.
    // TODO(stevegolton): Potential performance improvement to be obtained from
    // using the prepare statement API rather than a LIMIT 0 query.
    const query = `
      WITH data AS (
        ${data.sqlSource}
      )
      SELECT *
      FROM data
      LIMIT 0
    `;

    const result = await engine.query(query);
    rawColumns = result.columns();
  }

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
    rawColumns.map((c) => `${c} as ${RAW_PREFIX}${c}`),
    pivotCol && `${pivotCol} as pivot`,
    argSetIdColumn && `${argSetIdColumn} as arg_set_id`,
    colorCol && `${colorCol} as color`,
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

  return await createPerfettoTable({engine, name: tableName, as: query});
}

async function addPivotedSliceTracks(
  trace: Trace,
  tableName: string,
  titleBase: string,
  uriBase: string,
  pivotColName: string,
  colorCol?: string,
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
    const name = `${titleBase}: ${pivotColName} = ${sqlValueToReadableString(pivotValue)}`;

    const schema = {
      id: NUM,
      ts: LONG,
      dur: LONG,
      name: STR,
      ...(colorCol && {color: UNKNOWN}),
    };

    trace.tracks.registerTrack({
      uri,
      renderer: SliceTrack.create({
        trace,
        uri,
        dataset: new SourceDataset({
          schema,
          src: tableName,
          filter: {
            col: 'pivot',
            eq: pivotValue,
          },
        }),
        colorizer: (row) =>
          getColorForSlice(sqlValueToReadableString(row.color) ?? row.name),
        detailsPanel: (row) => {
          return new DebugSliceTrackDetailsPanel(trace, tableName, row.id);
        },
      }),
    });

    const trackNode = new TrackNode({uri, name, removable: true});
    trace.currentWorkspace.pinnedTracksNode.addChildLast(trackNode);
  }
}

function addSingleSliceTrack(
  trace: Trace,
  tableName: string,
  name: string,
  uri: string,
  argSetIdCol?: string,
  colorCol?: string,
) {
  const schema = {
    id: NUM,
    ts: LONG,
    dur: LONG,
    name: STR,
    ...(colorCol && {color: UNKNOWN}),
  };

  trace.tracks.registerTrack({
    uri,
    renderer: SliceTrack.create({
      trace,
      uri,
      dataset: new SourceDataset({
        schema,
        src: tableName,
      }),
      colorizer: (row) =>
        getColorForSlice(sqlValueToReadableString(row.color) ?? row.name),
      detailsPanel: (row) => {
        return new DebugSliceTrackDetailsPanel(
          trace,
          tableName,
          row.id,
          argSetIdCol,
        );
      },
    }),
  });

  const trackNode = new TrackNode({uri, name, removable: true});
  trace.currentWorkspace.pinnedTracksNode.addChildLast(trackNode);
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

  return await createPerfettoTable({engine, name: tableName, as: query});
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
    const name = `${titleBase}: ${pivotColName} = ${sqlValueToReadableString(pivotValue)}`;

    trace.tracks.registerTrack({
      uri,
      renderer: new SqlTableCounterTrack(
        trace,
        uri,
        `
          SELECT *
          FROM ${tableName}
          WHERE pivot = ${sqlValueToSqliteString(pivotValue)}
        `,
      ),
    });

    const trackNode = new TrackNode({uri, name, removable: true});
    trace.currentWorkspace.pinnedTracksNode.addChildLast(trackNode);
  }
}

function addSingleCounterTrack(
  trace: Trace,
  tableName: string,
  name: string,
  uri: string,
) {
  trace.tracks.registerTrack({
    uri,
    renderer: new SqlTableCounterTrack(trace, uri, tableName),
  });

  const trackNode = new TrackNode({uri, name, removable: true});
  trace.currentWorkspace.pinnedTracksNode.addChildLast(trackNode);
}
