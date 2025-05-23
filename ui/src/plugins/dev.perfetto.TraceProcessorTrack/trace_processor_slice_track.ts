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
import {assertTrue} from '../../base/logging';
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
import {Engine} from '../../trace_processor/engine';
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

const schema = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  name: STR_NULL,
  depth: NUM,
  thread_dur: LONG_NULL,
  category: STR_NULL,
};

export async function createTraceProcessorSliceTrack({
  trace,
  uri,
  maxDepth,
  trackIds,
  detailsPanel,
}: TraceProcessorSliceTrackAttrs) {
  return new DatasetSliceTrack({
    trace,
    uri,
    dataset: await getDataset(trace.engine, trackIds),
    sliceName: (row) => (row.name === null ? '[null]' : row.name),
    initialMaxDepth: maxDepth,
    rootTableName: 'slice',
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

async function getDataset(engine: Engine, trackIds: ReadonlyArray<number>) {
  assertTrue(trackIds.length > 0);

  if (trackIds.length === 1) {
    return new SourceDataset({
      schema,
      src: 'slice',
      filter: {
        col: 'track_id',
        eq: trackIds[0],
      },
    });
  } else {
    // If we have more than one trackId, we must use experimental_slice_layout
    // to work out the depths. However, just using this as the dataset can be
    // extremely slow. So we cache the depths up front in a new table for this
    // track.
    const tableName = `__async_slice_depth_${trackIds[0]}`;

    await engine.query(`
      create perfetto table ${tableName} as
      select
        id,
        layout_depth as depth
      from experimental_slice_layout
      where filter_track_ids = '${trackIds.join(',')}'
    `);

    // The (inner) join acts as a filter as well as providing the depth.
    return new SourceDataset({
      schema,
      src: `
        select
          slice.id,
          ts,
          dur,
          d.depth as depth,
          name,
          thread_dur,
          track_id,
          category
        from slice
        join ${tableName} d using (id)
      `,
    });
  }
}
