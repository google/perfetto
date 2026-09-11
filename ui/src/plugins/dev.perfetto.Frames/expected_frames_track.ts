// Copyright (C) 2023 The Android Open Source Project
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

import {HSLColor} from '../../base/color';
import {makeColorScheme} from '../../components/colorizer';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import {SliceTrack} from '../../components/tracks/slice_track';
import {ThreadSliceDetailsPanel} from '../../components/details/thread_slice_details_tab';
import {sqlValueToSqliteString} from '../../trace_processor/sql_utils';

const GREEN = makeColorScheme(new HSLColor('#4CAF50')); // Green 500

/**
 * Creates a track renderer for Expected Frame Timeline slices.
 *
 * @param trace - The trace context.
 * @param uri - Unique URI for the track.
 * @param maxDepth - Maximum visual depth of slices on the track.
 * @param trackIds - Track IDs containing frame timeline slices for this process.
 * @param layer - Optional layer filter. When provided, scopes the track to
 * frames belonging to this specific layer and process.
 * @param layer.name - Name of the layer.
 * @param layer.upid - Process upid owning the layer.
 */
export function createExpectedFramesTrack(
  trace: Trace,
  uri: string,
  maxDepth: number,
  trackIds: ReadonlyArray<number>,
  layer?: {readonly name: string; readonly upid: number},
) {
  const src =
    layer === undefined
      ? 'expected_frame_timeline_slice'
      : expectedFramesForLayer(layer.name, layer.upid);
  return SliceTrack.create({
    trace,
    uri,
    initialMaxDepth: maxDepth,
    rootTableName: 'slice',
    dataset: new SourceDataset({
      src,
      schema: {
        ts: LONG,
        dur: LONG,
        name: STR,
        id: NUM,
        track_id: NUM,
        arg_set_id: NUM_NULL,
      },
      filter: {
        col: 'track_id',
        in: trackIds,
      },
    }),
    colorizer: () => GREEN,
    detailsPanel: () => new ThreadSliceDetailsPanel(trace),
  });
}

// Expected frames are usually tagged with the name of the layer they belong to,
// but some traces only emit them for one of the layers of a process. Frames
// tagged with another layer name are attributed to this layer via the surface
// frame token of the corresponding actual frame, which is unique in a process.
function expectedFramesForLayer(layerName: string, upid: number): string {
  const name = sqlValueToSqliteString(layerName);
  return `
    select *
    from expected_frame_timeline_slice
    where layer_name = ${name} and upid = ${upid}

    union

    select exp.*
    from actual_frame_timeline_slice act
    join expected_frame_timeline_slice exp
      on act.upid = exp.upid
      and act.surface_frame_token = exp.surface_frame_token
    where act.layer_name = ${name}
      and act.upid = ${upid}
      and act.surface_frame_token is not null
  `;
}
