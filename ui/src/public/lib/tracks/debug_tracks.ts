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

import {
  matchesSqlValue,
  sqlValueToReadableString,
} from '../../../trace_processor/sql_utils';
import {TrackNode} from '../../workspace';
import {Trace} from '../../trace';
import {
  createQuerySliceTrack,
  SliceColumnMapping,
  SqlDataSource,
} from './query_slice_track';
import {
  CounterColumnMapping,
  createQueryCounterTrack,
} from './query_counter_track';

let trackCounter = 0; // For reproducible ids.

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

export interface DebugSliceTrackArgs {
  readonly trace: Trace;
  readonly data: SqlDataSource;
  readonly title?: string;
  readonly columns?: Partial<SliceColumnMapping>;
  readonly argColumns?: string[];
}

/**
 * Adds a new debug slice track to the workspace.
 *
 * See {@link createQuerySliceTrack} for details about the configuration args.
 *
 * A debug slice track is a track based on a query which is:
 * - Based on a query.
 * - Uses automatic slice layout.
 * - Automatically added to the top of the current workspace.
 * - Pinned.
 * - Has a close button.
 */
export async function addDebugSliceTrack(args: DebugSliceTrackArgs) {
  const trace = args.trace;
  const cnt = trackCounter++;
  const uri = `debugSliceTrack/${cnt}`;
  const title = args.title?.trim() || `Debug Slice Track ${cnt}`;

  // Create & register the track renderer
  const track = await createQuerySliceTrack({...args, uri});
  trace.tracks.registerTrack({uri, title, track});

  // Create the track node and pin it
  const trackNode = new TrackNode({uri, title, removable: true});
  trace.workspace.addChildFirst(trackNode);
  trackNode.pin();
}

export interface DebugCounterTrackArgs {
  readonly trace: Trace;
  readonly data: SqlDataSource;
  readonly title?: string;
  readonly columns?: Partial<CounterColumnMapping>;
}

/**
 * Adds a new debug counter track to the workspace.
 *
 * See {@link createQueryCounterTrack} for details about the configuration args.
 *
 * A debug counter track is a track based on a query which is:
 * - Based on a query.
 * - Automatically added to the top of the current workspace.
 * - Pinned.
 * - Has a close button.
 */
export async function addDebugCounterTrack(args: DebugCounterTrackArgs) {
  const trace = args.trace;
  const cnt = trackCounter++;
  const uri = `debugCounterTrack/${cnt}`;
  const title = args.title?.trim() || `Debug Counter Track ${cnt}`;

  // Create & register the track renderer
  const track = await createQueryCounterTrack({...args, uri});
  trace.tracks.registerTrack({uri, title, track});

  // Create the track node and pin it
  const trackNode = new TrackNode({uri, title, removable: true});
  trace.workspace.addChildFirst(trackNode);
  trackNode.pin();
}
