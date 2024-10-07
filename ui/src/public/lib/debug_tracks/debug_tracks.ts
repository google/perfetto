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

import {DebugSliceTrack} from './slice_track';
import {
  createPerfettoTable,
  matchesSqlValue,
  sqlValueToReadableString,
} from '../../../trace_processor/sql_utils';
import {DebugCounterTrack} from './counter_track';
import {ARG_PREFIX, DebugSliceDetailsPanel} from './details_tab';
import {TrackNode} from '../../workspace';
import {Trace} from '../../trace';
import {TrackEventSelection} from '../../selection';

let trackCounter = 0; // For reproducible ids.

// Names of the columns of the underlying view to be used as
// ts / dur / name / pivot.
export interface SliceColumns {
  ts: string;
  dur: string;
  name: string;
}

let debugTrackCount = 0;

export interface SqlDataSource {
  // SQL source selecting the necessary data.
  sqlSource: string;

  // Optional: Rename columns from the query result.
  // If omitted, original column names from the query are used instead.
  // The caller is responsible for ensuring that the number of items in this
  // list matches the number of columns returned by sqlSource.
  columns?: string[];
}

// Creates actions to add a debug track. The actions must be dispatched to
// have an effect. Use this variant if you want to create many tracks at
// once or want to tweak the actions once produced. Otherwise, use
// addDebugSliceTrack().
function addDebugTrack(trace: Trace, trackName: string, uri: string): void {
  const debugTrackId = ++debugTrackCount;
  const title = trackName.trim() || `Debug Track ${debugTrackId}`;
  const track = new TrackNode({uri, title});
  trace.workspace.addChildFirst(track);
  track.pin();
}

export async function addPivotedTracks(
  trace: Trace,
  data: SqlDataSource,
  trackName: string,
  pivotColumn: string,
  createTrack: (
    trace: Trace,
    data: SqlDataSource,
    trackName: string,
  ) => Promise<void>,
) {
  const iter = (
    await trace.engine.query(`
    with all_vals as (${data.sqlSource})
    select DISTINCT ${pivotColumn} from all_vals
    order by ${pivotColumn}
  `)
  ).iter({});

  for (; iter.valid(); iter.next()) {
    await createTrack(
      trace,
      {
        sqlSource: `select * from
        (${data.sqlSource})
        where ${pivotColumn} ${matchesSqlValue(iter.get(pivotColumn))}`,
      },
      `${trackName.trim() || 'Pivot Track'}: ${sqlValueToReadableString(iter.get(pivotColumn))}`,
    );
  }
}

// Adds a debug track immediately. Use createDebugSliceTrackActions() if you
// want to create many tracks at once.
export async function addDebugSliceTrack(
  trace: Trace,
  data: SqlDataSource,
  trackName: string,
  sliceColumns: SliceColumns,
  argColumns: string[],
): Promise<void> {
  const cnt = trackCounter++;
  // Create a new table from the debug track definition. This will be used as
  // the backing data source for our track and its details panel.
  const tableName = `__debug_slice_${cnt}`;

  // TODO(stevegolton): Right now we ignore the AsyncDisposable that this
  // function returns, and so never clean up this table. The problem is we have
  // no where sensible to do this cleanup.
  // - If we did it in the track's onDestroy function, we could drop the table
  //   while the details panel still needs access to it.
  // - If we did it in the plugin's onTraceUnload function, we could risk
  //   dropping it n the middle of a track update cycle as track lifecycles are
  //   not synchronized with plugin lifecycles.
  await createPerfettoTable(
    trace.engine,
    tableName,
    createDebugSliceTrackTableExpr(data, sliceColumns, argColumns),
  );

  const uri = `debug.slice.${cnt}`;
  trace.tracks.registerTrack({
    uri,
    title: trackName,
    track: new DebugSliceTrack(trace, {trackUri: uri}, tableName),
    detailsPanel: (sel: TrackEventSelection) => {
      return new DebugSliceDetailsPanel(trace, tableName, sel.eventId);
    },
  });

  // Create the actions to add this track to the tracklist
  addDebugTrack(trace, trackName, uri);
}

function createDebugSliceTrackTableExpr(
  data: SqlDataSource,
  sliceColumns: SliceColumns,
  argColumns: string[],
): string {
  const dataColumns =
    data.columns !== undefined ? `(${data.columns.join(', ')})` : '';
  const dur = sliceColumns.dur === '0' ? 0 : sliceColumns.dur;
  return `
    with data${dataColumns} as (
      ${data.sqlSource}
    ),
    prepared_data as (
      select
        ${sliceColumns.ts} as ts,
        ifnull(cast(${dur} as int), -1) as dur,
        printf('%s', ${sliceColumns.name}) as name
        ${argColumns.length > 0 ? ',' : ''}
        ${argColumns.map((c) => `${c} as ${ARG_PREFIX}${c}`).join(',\n')}
      from data
    )
    select
      row_number() over (order by ts) as id,
      *
    from prepared_data
    order by ts
  `;
}

// Names of the columns of the underlying view to be used as ts / dur / name.
export interface CounterColumns {
  ts: string;
  value: string;
}

export interface CounterDebugTrackConfig {
  data: SqlDataSource;
  columns: CounterColumns;
}

export interface CounterDebugTrackCreateConfig {
  pinned?: boolean; // default true
  closeable?: boolean; // default true
}

// Adds a debug track immediately. Use createDebugCounterTrackActions() if you
// want to create many tracks at once.
export async function addDebugCounterTrack(
  trace: Trace,
  data: SqlDataSource,
  trackName: string,
  columns: CounterColumns,
): Promise<void> {
  const cnt = trackCounter++;
  // Create a new table from the debug track definition. This will be used as
  // the backing data source for our track and its details panel.
  const tableName = `__debug_counter_${cnt}`;

  // TODO(stevegolton): Right now we ignore the AsyncDisposable that this
  // function returns, and so never clean up this table. The problem is we have
  // no where sensible to do this cleanup.
  // - If we did it in the track's onDestroy function, we could drop the table
  //   while the details panel still needs access to it.
  // - If we did it in the plugin's onTraceUnload function, we could risk
  //   dropping it n the middle of a track update cycle as track lifecycles are
  //   not synchronized with plugin lifecycles.
  await createPerfettoTable(
    trace.engine,
    tableName,
    createDebugCounterTrackTableExpr(data, columns),
  );

  const uri = `debug.counter.${cnt}`;
  trace.tracks.registerTrack({
    uri,
    title: trackName,
    track: new DebugCounterTrack(trace, {trackUri: uri}, tableName),
  });

  // Create the actions to add this track to the tracklist
  addDebugTrack(trace, trackName, uri);
}

function createDebugCounterTrackTableExpr(
  data: SqlDataSource,
  columns: CounterColumns,
): string {
  return `
    with data as (
      ${data.sqlSource}
    )
    select
      ${columns.ts} as ts,
      ${columns.value} as value
    from data
    order by ts
  `;
}
