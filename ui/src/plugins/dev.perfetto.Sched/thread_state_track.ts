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

import {colorForState} from '../../components/colorizer';
import {DatasetSliceTrack} from '../../components/tracks/dataset_slice_track';
import {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import {ThreadStateDetailsPanel} from './thread_state_details_panel';

export function createThreadStateTrack(
  trace: Trace,
  uri: string,
  utid: number,
) {
  return new DatasetSliceTrack({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        layer: NUM,
        cpu: NUM_NULL,
        state: STR,
        io_wait: NUM_NULL,
        utid: NUM,
        name: STR,
        depth: NUM,
      },
      src: `
        SELECT
          id,
          ts,
          dur,
          cpu,
          state,
          io_wait,
          utid,
          sched_state_io_to_human_readable_string(state, io_wait) AS name,
          -- Move sleeping and idle slices to the back layer, others on top
          CASE
            WHEN state IN ('S', 'I') THEN 0
            ELSE 1
          END AS layer,
          0 AS depth
        FROM thread_state
      `,
      filter: {
        col: 'utid',
        eq: utid,
      },
    }),
    // Make thread slice tracks a little shorter in height.
    sliceLayout: {
      sliceHeight: 12,
      titleSizePx: 10,
    },
    colorizer: (row) => colorForState(row.name),
    detailsPanel: (row) => new ThreadStateDetailsPanel(trace, row.id),
    rootTableName: 'thread_state',
  });
}
