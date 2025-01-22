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

import {BigintMath as BIMath} from '../../base/bigint_math';
import {clamp} from '../../base/math_utils';
import {LONG, LONG_NULL, NUM, STR} from '../../trace_processor/query_result';
import {ThreadSliceDetailsPanel} from '../details/thread_slice_details_tab';
import {TraceImpl} from '../../core/trace_impl';
import {assertIsInstance} from '../../base/logging';
import {Trace} from '../../public/trace';
import {DatasetSliceTrack} from './dataset_slice_track';
import {SourceDataset} from '../../trace_processor/dataset';

export function createThreadSliceTrack(
  trace: Trace,
  uri: string,
  trackId: number,
  maxDepth: number,
  tableName: string = 'slice',
) {
  return new DatasetSliceTrack({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        depth: NUM,
        name: STR,
        threadDur: LONG_NULL,
      },
      src: `
        SELECT
          id,
          ts,
          dur,
          depth,
          ifnull(name, '') as name,
          thread_dur as threadDur,
          track_id
        FROM ${tableName}
      `,
      filter: {
        col: 'track_id',
        eq: trackId,
      },
    }),
    initialMaxDepth: maxDepth,
    rootTableName: tableName,
    detailsPanel: () => {
      // Rationale for the assertIsInstance: ThreadSliceDetailsPanel requires a
      // TraceImpl (because of flows) but here we must take a Trace interface,
      // because this track is exposed to plugins (which see only Trace).
      return new ThreadSliceDetailsPanel(assertIsInstance(trace, TraceImpl));
    },
    fillRatio: (row) => {
      if (row.dur > 0n && row.threadDur !== null) {
        return clamp(BIMath.ratio(row.threadDur, row.dur), 0, 1);
      } else {
        return 1;
      }
    },
  });
}
