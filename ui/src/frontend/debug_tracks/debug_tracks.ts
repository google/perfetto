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

import {uuidv4, uuidv4Sql} from '../../base/uuid';
import {Actions, DeferredAction} from '../../common/actions';
import {PrimaryTrackSortKey, SCROLLING_TRACK_GROUP} from '../../common/state';
import {globals} from '../globals';
import {TrackDescriptor} from '../../public';
import {DebugSliceTrack} from './slice_track';
import {
  createPerfettoTable,
  matchesSqlValue,
} from '../../trace_processor/sql_utils';
import {Engine} from '../../trace_processor/engine';
import {DebugCounterTrack} from './counter_track';
import {ARG_PREFIX} from './details_tab';

// We need to add debug tracks from the core and from plugins. In order to add a
// debug track we need to pass a context through with we can add the track. This
// is different for plugins vs the core. This interface defines the generic
// shape of this context, which can be supplied from a plugin or built from
// globals.
//
// TODO(stevegolton): In the future, both the core and plugins should
// have access to some Context object which implements the various things we
// want to do in a generic way, so that we don't have to do this mangling to get
// this to work.
interface Context {
  engine: Engine;
  registerTrack(track: TrackDescriptor): unknown;
}

// Names of the columns of the underlying view to be used as
// ts / dur / name / pivot.
export interface SliceColumns {
  ts: string;
  dur: string;
  name: string;
  pivot?: string;
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
function createAddDebugTrackActions(
  trackName: string,
  uri: string,
): DeferredAction<{}>[] {
  const debugTrackId = ++debugTrackCount;
  const trackKey = uuidv4();

  const actions: DeferredAction<{}>[] = [
    Actions.addTrack({
      key: trackKey,
      name: trackName.trim() || `Debug Track ${debugTrackId}`,
      uri,
      trackSortKey: PrimaryTrackSortKey.DEBUG_TRACK,
      trackGroup: SCROLLING_TRACK_GROUP,
      closeable: true,
    }),
    Actions.toggleTrackPinned({trackKey}),
  ];

  return actions;
}

export async function addPivotDebugSliceTracks(
  ctx: Context,
  data: SqlDataSource,
  trackName: string,
  sliceColumns: SliceColumns,
  argColumns: string[],
) {
  if (sliceColumns.pivot) {
    // Get distinct values to group by
    const pivotValues = await ctx.engine.query(`
      with all_vals as (${data.sqlSource})
      select DISTINCT ${sliceColumns.pivot} from all_vals;`);

    const iter = pivotValues.iter({});

    for (; iter.valid(); iter.next()) {
      const pivotDataSource: SqlDataSource = {
        sqlSource: `select * from
        (${data.sqlSource})
        where ${sliceColumns.pivot} ${matchesSqlValue(
          iter.get(sliceColumns.pivot),
        )}`,
      };

      await addDebugSliceTrack(
        ctx,
        pivotDataSource,
        `${trackName.trim() || 'Pivot Track'}: ${iter.get(sliceColumns.pivot)}`,
        sliceColumns,
        argColumns,
      );
    }
  }
}

// Adds a debug track immediately. Use createDebugSliceTrackActions() if you
// want to create many tracks at once.
export async function addDebugSliceTrack(
  ctx: Context,
  data: SqlDataSource,
  trackName: string,
  sliceColumns: SliceColumns,
  argColumns: string[],
): Promise<void> {
  // Create a new table from the debug track definition. This will be used as
  // the backing data source for our track and its details panel.
  const tableName = `__debug_slice_${uuidv4Sql()}`;

  // TODO(stevegolton): Right now we ignore the AsyncDisposable that this
  // function returns, and so never clean up this table. The problem is we have
  // no where sensible to do this cleanup.
  // - If we did it in the track's onDestroy function, we could drop the table
  //   while the details panel still needs access to it.
  // - If we did it in the plugin's onTraceUnload function, we could risk
  //   dropping it n the middle of a track update cycle as track lifecycles are
  //   not synchronized with plugin lifecycles.
  await createPerfettoTable(
    ctx.engine,
    tableName,
    createDebugSliceTrackTableExpr(data, sliceColumns, argColumns),
  );

  const uri = `debug.slice.${uuidv4()}`;
  ctx.registerTrack({
    uri,
    trackFactory: (trackCtx) => {
      return new DebugSliceTrack(ctx.engine, trackCtx, tableName);
    },
  });

  // Create the actions to add this track to the tracklist
  const actions = await createAddDebugTrackActions(trackName, uri);
  globals.dispatchMultiple(actions);
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
  ctx: Context,
  data: SqlDataSource,
  trackName: string,
  columns: CounterColumns,
): Promise<void> {
  // Create a new table from the debug track definition. This will be used as
  // the backing data source for our track and its details panel.
  const tableName = `__debug_counter_${uuidv4Sql()}`;

  // TODO(stevegolton): Right now we ignore the AsyncDisposable that this
  // function returns, and so never clean up this table. The problem is we have
  // no where sensible to do this cleanup.
  // - If we did it in the track's onDestroy function, we could drop the table
  //   while the details panel still needs access to it.
  // - If we did it in the plugin's onTraceUnload function, we could risk
  //   dropping it n the middle of a track update cycle as track lifecycles are
  //   not synchronized with plugin lifecycles.
  await createPerfettoTable(
    ctx.engine,
    tableName,
    createDebugCounterTrackTableExpr(data, columns),
  );

  const uri = `debug.counter.${uuidv4()}`;
  ctx.registerTrack({
    uri,
    trackFactory: (trackCtx) => {
      return new DebugCounterTrack(ctx.engine, trackCtx, tableName);
    },
  });

  // Create the actions to add this track to the tracklist
  const actions = await createAddDebugTrackActions(trackName, uri);
  globals.dispatchMultiple(actions);
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
