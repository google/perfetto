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

import {LONG, NUM} from '../../trace_processor/query_result';
import {CpuProfileSampleFlamegraphDetailsPanel} from './cpu_profile_details_panel';
import {Trace} from '../../public/trace';
import {DatasetSliceTrack} from '../../components/tracks/dataset_slice_track';
import {PartitionedDataset, SourceDataset} from '../../trace_processor/dataset';
import {Time} from '../../base/time';
import {getColorForSample} from '../../components/colorizer';

const cpuProfileStackSampleTable = new SourceDataset({
  src: 'cpu_profile_stack_sample',
  schema: {
    id: NUM,
    ts: LONG,
    callsite_id: NUM,
    utid: NUM,
  },
});

export function createCpuProfileTrack(trace: Trace, uri: string, utid: number) {
  return new DatasetSliceTrack({
    trace,
    uri,
    dataset: new PartitionedDataset({
      base: cpuProfileStackSampleTable,
      schema: {
        id: NUM,
        ts: LONG,
        callsite_id: NUM,
      },
      partition: {
        col: 'utid',
        eq: utid,
      },
    }),
    sliceName: () => 'CPU Sample',
    colorizer: (row) => getColorForSample(row.callsite_id),
    detailsPanel: (row) => {
      return new CpuProfileSampleFlamegraphDetailsPanel(
        trace,
        Time.fromRaw(row.ts),
        utid,
      );
    },
  });
}
