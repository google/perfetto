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

import {Utid} from '../../components/sql_utils/core_types';
import {SliceTrack} from '../../components/tracks/slice_track';
import {Trace} from '../../public/trace';
import {ChromeTasksDetailsPanel} from './details';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {SourceDataset} from '../../trace_processor/dataset';

export function createChromeTasksThreadTrack(
  trace: Trace,
  uri: string,
  utid: Utid,
) {
  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        name: STR,
      },
      src: 'chrome_tasks',
      filter: {
        col: 'utid',
        eq: utid,
      },
    }),
    detailsPanel: (row) => new ChromeTasksDetailsPanel(trace, row.id),
  });
}
