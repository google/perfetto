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

import {uuidv4} from '../base/uuid';
import {Actions, DeferredAction} from '../common/actions';
import {SCROLLING_TRACK_GROUP} from '../common/state';
import {globals} from './globals';
import {EngineProxy, PrimaryTrackSortKey} from '../public';

export const ARG_PREFIX = 'arg_';
export const DEBUG_SLICE_TRACK_URI = 'perfetto.DebugSlices';

// Names of the columns of the underlying view to be used as ts / dur / name.
export interface SliceColumns {
  ts: string;
  dur: string;
  name: string;
}

export interface DebugTrackV2CreateConfig {
  pinned?: boolean;     // default true
  closeable?: boolean;  // default true
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
export async function createDebugSliceTrackActions(
  engine: EngineProxy,
  data: SqlDataSource,
  trackName: string,
  sliceColumns: SliceColumns,
  argColumns: string[],
  config?: DebugTrackV2CreateConfig): Promise<DeferredAction<{}>[]> {
  // To prepare displaying the provided data as a track, materialize it and
  // compute depths.
  const debugTrackId = ++debugTrackCount;
  const sqlTableName = `__debug_slice_${debugTrackId}`;

  // If the view has clashing names (e.g. "name" coming from joining two
  // different tables, we will see names like "name_1", "name_2", but they won't
  // be addressable from the SQL. So we explicitly name them through a list of
  // columns passed to CTE.
  const dataColumns =
      data.columns !== undefined ? `(${data.columns.join(', ')})` : '';

  // TODO(altimin): Support removing this table when the track is closed.
  const dur = sliceColumns.dur === '0' ? 0 : sliceColumns.dur;
  await engine.query(`
      create table ${sqlTableName} as
      with data${dataColumns} as (
        ${data.sqlSource}
      ),
      prepared_data as (
        select
          row_number() over () as id,
          ${sliceColumns.ts} as ts,
          ifnull(cast(${dur} as int), -1) as dur,
          printf('%s', ${sliceColumns.name}) as name
          ${argColumns.length > 0 ? ',' : ''}
          ${argColumns.map((c) => `${c} as ${ARG_PREFIX}${c}`).join(',\n')}
        from data
      )
      select
        *
      from prepared_data
      order by ts;`);

  const closeable = config?.closeable ?? true;
  const trackKey = uuidv4();
  const actions: DeferredAction<{}>[] = [
    Actions.addTrack({
      key: trackKey,
      name: trackName.trim() || `Debug Track ${debugTrackId}`,
      uri: DEBUG_SLICE_TRACK_URI,
      trackSortKey: PrimaryTrackSortKey.DEBUG_TRACK,
      trackGroup: SCROLLING_TRACK_GROUP,
      params: {
        sqlTableName,
        columns: sliceColumns,
        closeable,
      },
    }),
  ];
  if (config?.pinned ?? true) {
    actions.push(Actions.toggleTrackPinned({trackKey}));
  }
  return actions;
}

// Adds a debug track immediately. Use createDebugSliceTrackActions() if you
// want to create many tracks at once.
export async function addDebugSliceTrack(
  engine: EngineProxy,
  data: SqlDataSource,
  trackName: string,
  sliceColumns: SliceColumns,
  argColumns: string[],
  config?: DebugTrackV2CreateConfig) {
  const actions = await createDebugSliceTrackActions(
    engine, data, trackName, sliceColumns, argColumns, config);
  globals.dispatchMultiple(actions);
}
