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

import m from 'mithril';
import {Button} from '../../widgets/button';
import {Icons} from '../../base/semantic_icons';
import {uuidv4Sql} from '../../base/uuid';
import {createView} from '../../trace_processor/sql_utils';
import {Trace} from '../../public/trace';
import {DatasetSliceTrack} from './dataset_slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, LONG_NULL, NUM, STR} from '../../trace_processor/query_result';
import {ThreadSliceDetailsPanel} from '../details/thread_slice_details_tab';
import {BigintMath} from '../../base/bigint_math';
import {clamp} from '../../base/math_utils';

export interface VisualizedArgsTrackAttrs {
  readonly uri: string;
  readonly trace: Trace;
  readonly trackId: number;
  readonly maxDepth: number;
  readonly argName: string;
  readonly onClose: () => void;
}

export async function createVisualizedArgsTrack({
  uri,
  trace,
  trackId,
  maxDepth,
  argName,
  onClose,
}: VisualizedArgsTrackAttrs) {
  const uuid = uuidv4Sql();
  const escapedArgName = argName.replace(/[^a-zA-Z]/g, '_');
  const viewName = `__arg_visualisation_helper_${escapedArgName}_${uuid}_slice`;

  await createView(
    trace.engine,
    viewName,
    `
      with slice_with_arg as (
        select
          slice.id,
          slice.track_id,
          slice.ts,
          slice.dur,
          slice.thread_dur,
          NULL as cat,
          args.display_value as name
        from slice
        join args using (arg_set_id)
        where args.key='${argName}'
      )
      select
        *,
        (select count()
        from ancestor_slice(s1.id) s2
        join slice_with_arg s3 on s2.id=s3.id
        ) as depth
      from slice_with_arg s1
      order by id
    `,
  );

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
        thread_dur: LONG_NULL,
      },
      src: viewName,
      filter: {
        col: 'track_id',
        eq: trackId,
      },
    }),
    initialMaxDepth: maxDepth,
    detailsPanel: () => new ThreadSliceDetailsPanel(trace),
    fillRatio: (row) => {
      if (row.dur > 0n && row.thread_dur !== null) {
        return clamp(BigintMath.ratio(row.thread_dur, row.dur), 0, 1);
      } else {
        return 1;
      }
    },
    shellButtons: () => {
      return m(Button, {
        onclick: onClose,
        icon: Icons.Close,
        title: 'Close all visualised args tracks for this arg',
        compact: true,
      });
    },
  });
}
