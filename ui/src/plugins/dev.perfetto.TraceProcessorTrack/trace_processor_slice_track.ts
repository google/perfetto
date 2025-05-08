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

import {BigintMath as BIMath} from '../../base/bigint_math';
import {clamp} from '../../base/math_utils';
import {exists} from '../../base/utils';
import {ThreadSliceDetailsPanel} from '../../components/details/thread_slice_details_tab';
import {
  DatasetSliceTrack,
  renderTooltip,
} from '../../components/tracks/dataset_slice_track';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  LONG,
  LONG_NULL,
  NUM,
  STR_NULL,
} from '../../trace_processor/query_result';
import m from 'mithril';

export interface TraceProcessorSliceTrackAttrs {
  readonly trace: Trace;
  readonly uri: string;
  readonly maxDepth?: number;
  readonly trackIds: ReadonlyArray<number>;
  readonly detailsPanel?: (row: {id: number}) => TrackEventDetailsPanel;
}

export function createTraceProcessorSliceTrack({
  trace,
  uri,
  maxDepth,
  trackIds,
  detailsPanel,
}: TraceProcessorSliceTrackAttrs) {
  return new DatasetSliceTrack({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        name: STR_NULL,
        depth: NUM,
        thread_dur: LONG_NULL,
        category: STR_NULL,
      },
      src: 'slice',
      filter: {
        col: 'track_id',
        in: trackIds,
      },
    }),
    sliceName: (row) => (row.name === null ? '[null]' : row.name),
    initialMaxDepth: maxDepth,
    rootTableName: 'slice',
    queryGenerator: getDepthProvider(trackIds),
    fillRatio: (row) => {
      if (row.dur > 0n && row.thread_dur !== null) {
        return clamp(BIMath.ratio(row.thread_dur, row.dur), 0, 1);
      } else {
        return 1;
      }
    },
    tooltip: (slice) => {
      return renderTooltip(trace, slice, {
        title: slice.title,
        extras:
          exists(slice.row.category) && m('', 'Category: ', slice.row.category),
      });
    },
    detailsPanel: detailsPanel
      ? (row) => detailsPanel(row)
      : () => new ThreadSliceDetailsPanel(trace),
  });
}

function getDepthProvider(trackIds: ReadonlyArray<number>) {
  // If we have more than one track we basically just need to replace the query
  // used for rendering tracks with this one which uses
  // experimental_slice_layout. The reason we don't just put this query in the
  // dataset is that the dataset is shared with the outside world and we don't
  // want to force everyone else to use experimental_slice_track.
  // TODO(stevegolton): Let's teach internal_layout how to mimic this behaviour.
  if (trackIds.length > 1) {
    return () => `
      select
        id,
        ts,
        dur,
        layout_depth as depth,
        name,
        thread_dur,
        category
      from experimental_slice_layout
      where filter_track_ids = '${trackIds.join(',')}'
    `;
  } else {
    return undefined;
  }
}
