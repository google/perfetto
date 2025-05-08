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

import {DatasetSliceTrack} from '../../components/tracks/dataset_slice_track';
import {JANK_COLOR} from './jank_colors';
import {EventLatencySliceDetailsPanel} from './event_latency_details_panel';
import {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {getColorForSlice} from '../../components/colorizer';

export const JANKY_LATENCY_NAME = 'Janky EventLatency';

export function createEventLatencyTrack(
  trace: Trace,
  uri: string,
  baseTable: string,
) {
  return new DatasetSliceTrack({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        name: STR,
        depth: NUM,
      },
      src: baseTable,
    }),
    colorizer: (row) => {
      return row.name === JANKY_LATENCY_NAME
        ? JANK_COLOR
        : getColorForSlice(row.name);
    },
    detailsPanel: (row) => new EventLatencySliceDetailsPanel(trace, row.id),
  });
}
