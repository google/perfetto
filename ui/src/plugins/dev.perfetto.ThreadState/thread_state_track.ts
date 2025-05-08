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
import {LONG, NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import {translateState} from '../../components/sql_utils/thread_state';
import {ThreadStateDetailsPanel} from './thread_state_details_panel';
import {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {DatasetSliceTrack} from '../../components/tracks/dataset_slice_track';

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
        cpu: NUM_NULL,
        state: STR,
        io_wait: NUM_NULL,
        utid: NUM,
      },
      src: 'thread_state',
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
    queryGenerator: (dataset) => {
      // We actually abuse the depth provider here just a little. Instead of
      // providing just a depth value, we also filter out non-sleeping/idle
      // slices. In effect, we're using this function as a little escape hatch
      // to override the query that's used for track rendering.
      //
      // The reason we don't just filter out sleeping/idle slices in the main
      // dataset is because we don't want to filter the dataset exposed via
      // getDataset(), we only want to filter them out at the rendering stage.
      //
      // The reason we don't want to render these slices is slightly nuanced.
      // Essentially, if we render all slices and zoom out, the vast majority of
      // the track is covered by sleeping slices, and the important
      // runnable/running/etc slices are no longer rendered (effectively
      // sleeping slices always 'win' on every bucket) so we lost the important
      // detail. We could get around this if we had some way to tell the
      // algorithm to prioritize some slices over others.
      return `
        select
          0 as depth,
          *
        from (${dataset.query()})
        where state not in ('S', 'I')
      `;
    },
    colorizer: (row) => {
      const title = getState(row);
      return colorForState(title);
    },
    sliceName: (row) => {
      return getState(row);
    },
    detailsPanel: (row) => new ThreadStateDetailsPanel(trace, row.id),
    rootTableName: 'thread_state',
  });
}

function getState(row: {io_wait: number | null; state: string}) {
  const ioWait = row.io_wait === null ? undefined : Boolean(row.io_wait);
  return translateState(row.state, ioWait);
}
