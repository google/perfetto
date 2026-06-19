// Copyright (C) 2026 The Android Open Source Project
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

import {exists} from '../../base/utils';
import {getColorForSlice} from '../../components/colorizer';
import {ThreadSliceDetailsPanel} from '../../components/details/thread_slice_details_tab';
import {SliceTrack, renderTooltip} from '../../components/tracks/slice_track';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR_NULL} from '../../trace_processor/query_result';
import m from 'mithril';

export interface TraceProcessorStateTrackAttrs {
  readonly trace: Trace;
  readonly uri: string;
  readonly trackId: number;
  readonly detailsPanel?: (row: {id: number}) => TrackEventDetailsPanel;
}

const schema = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  value: STR_NULL,
  depth: NUM, // always 0 in SQL
  category: STR_NULL,
};

export async function createTraceProcessorStateTrack({
  trace,
  uri,
  trackId,
  detailsPanel,
}: TraceProcessorStateTrackAttrs) {
  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      schema,
      select: {
        id: 'id',
        ts: 'ts',
        dur: 'dur',
        depth: '0',
        value: 'value',
        category: 'category',
      },
      src: "(SELECT * FROM state WHERE value != '')",
      filter: {
        col: 'track_id',
        eq: trackId,
      },
    }),
    sliceName: (row) => (row.value === null ? '[null]' : row.value),
    initialMaxDepth: 0,
    rootTableName: 'state',
    fillRatio: () => 1,
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
    colorizer: (row) => {
      if (row.value) {
        return getColorForSlice(row.value);
      }
      return getColorForSlice(`${row.id}`);
    },
  });
}
