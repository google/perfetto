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
import {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import {SliceTrack} from '../../components/tracks/slice_track';
import {ThreadSliceDetailsPanel} from '../../components/details/thread_slice_details_tab';

const GREEN = makeColorScheme(new HSLColor('#4CAF50')); // Green 500

export function createExpectedFramesTrack(
  trace: Trace,
  uri: string,
  maxDepth: number,
  trackIds: ReadonlyArray<number>,
) {
  return SliceTrack.create({
    trace,
    uri,
    initialMaxDepth: maxDepth,
    rootTableName: 'slice',
    dataset: new SourceDataset({
      src: 'expected_frame_timeline_slice',
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
