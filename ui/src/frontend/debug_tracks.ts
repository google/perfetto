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
import {DebugTrackV2Config} from '../tracks/debug/slice_track';

export const ARG_PREFIX = 'arg_';
export const DEBUG_SLICE_TRACK_URI = 'perfetto.DebugSlices';
export const DEBUG_COUNTER_TRACK_URI = 'perfetto.DebugCounter';

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
  _engine: EngineProxy,
  data: SqlDataSource,
  trackName: string,
  sliceColumns: SliceColumns,
  argColumns: string[],
  config?: DebugTrackV2CreateConfig): Promise<DeferredAction<{}>[]> {
  const debugTrackId = ++debugTrackCount;
  const closeable = config?.closeable ?? true;
  const trackKey = uuidv4();

  const trackConfig: DebugTrackV2Config = {
    data,
    columns: sliceColumns,
    closeable,
    argColumns,
  };

  const actions: DeferredAction<{}>[] = [
    Actions.addTrack({
      key: trackKey,
      name: trackName.trim() || `Debug Track ${debugTrackId}`,
      uri: DEBUG_SLICE_TRACK_URI,
      trackSortKey: PrimaryTrackSortKey.DEBUG_TRACK,
      trackGroup: SCROLLING_TRACK_GROUP,
      params: trackConfig,
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

// Names of the columns of the underlying view to be used as ts / dur / name.
export interface CounterColumns {
  ts: string;
  value: string;
}


export interface CounterDebugTrackConfig {
  data: SqlDataSource;
  columns: CounterColumns;
  closeable: boolean;
}


export interface CounterDebugTrackCreateConfig {
  pinned?: boolean;     // default true
  closeable?: boolean;  // default true
}

// Creates actions to add a debug track. The actions must be dispatched to
// have an effect. Use this variant if you want to create many tracks at
// once or want to tweak the actions once produced. Otherwise, use
// addDebugCounterTrack().
export async function createDebugCounterTrackActions(
  data: SqlDataSource,
  trackName: string,
  columns: CounterColumns,
  config?: CounterDebugTrackCreateConfig) {
  // To prepare displaying the provided data as a track, materialize it and
  // compute depths.
  const debugTrackId = ++debugTrackCount;

  const closeable = config?.closeable ?? true;
  const params: CounterDebugTrackConfig = {
    data,
    columns,
    closeable,
  };

  const trackKey = uuidv4();
  const actions: DeferredAction<{}>[] = [
    Actions.addTrack({
      key: trackKey,
      uri: DEBUG_COUNTER_TRACK_URI,
      name: trackName.trim() || `Debug Track ${debugTrackId}`,
      trackSortKey: PrimaryTrackSortKey.DEBUG_TRACK,
      trackGroup: SCROLLING_TRACK_GROUP,
      params,
    }),
  ];
  if (config?.pinned ?? true) {
    actions.push(Actions.toggleTrackPinned({trackKey}));
  }
  return actions;
}

// Adds a debug track immediately. Use createDebugCounterTrackActions() if you
// want to create many tracks at once.
export async function addDebugCounterTrack(
  data: SqlDataSource,
  trackName: string,
  columns: CounterColumns,
  config?: CounterDebugTrackCreateConfig) {
  const actions = await createDebugCounterTrackActions(
    data, trackName, columns, config);
  globals.dispatchMultiple(actions);
}
