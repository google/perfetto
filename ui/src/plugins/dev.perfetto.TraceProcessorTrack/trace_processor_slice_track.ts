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
import {getColorForSlice} from '../../components/colorizer';
import {ThreadSliceDetailsPanel} from '../../components/details/thread_slice_details_tab';
import {SliceTrack, renderTooltip} from '../../components/tracks/slice_track';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {Engine} from '../../trace_processor/engine';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import m from 'mithril';
import {createPerfettoTable} from '../../trace_processor/sql_utils';

export interface TraceProcessorSliceTrackAttrs {
  readonly trace: Trace;
  readonly uri: string;
  readonly maxDepth?: number;
  readonly trackIds: ReadonlyArray<number>;
  readonly detailsPanel?: (row: {id: number}) => TrackEventDetailsPanel;
  readonly depthTableName?: string;
}

const schema = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  name: STR_NULL,
  depth: NUM,
  thread_dur: LONG_NULL,
  category: STR_NULL,
  correlation_id: STR_NULL,
  arg_set_id: NUM_NULL,
  parent_id: NUM_NULL,
  track_id: NUM,
};

export async function createTraceProcessorSliceTrack({
  trace,
  uri,
  maxDepth,
  trackIds,
  detailsPanel,
  depthTableName,
}: TraceProcessorSliceTrackAttrs) {
  return SliceTrack.create({
    trace,
    uri,
    dataset: await getDataset(trace.engine, trackIds, depthTableName),
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
    colorizer: (row) => {
      if (row.correlation_id) {
        return getColorForSlice(row.correlation_id, {
          stripTrailingDigits: false,
        });
      }
      if (row.name) {
        return getColorForSlice(row.name);
      }
      return getColorForSlice(`${row.id}`);
    },
  });
}

async function getDataset(
  engine: Engine,
  trackIds: ReadonlyArray<number>,
  depthTableName?: string,
) {
  assertTrue(trackIds.length > 0);

  if (trackIds.length === 1) {
    // Single track case - use depth directly from slice table
    return new SourceDataset({
      schema,
      select: {
        id: 'id',
        ts: 'ts',
        dur: 'dur',
        depth: 'depth',
        name: 'name',
        thread_dur: 'thread_dur',
        track_id: 'track_id',
        category: 'category',
        correlation_id: "extract_arg(arg_set_id, 'correlation_id')",
        arg_set_id: 'arg_set_id',
        parent_id: 'parent_id',
      },
      src: 'slice',
      filter: {
        col: 'track_id',
        eq: trackIds[0],
      },
    });
  } else {
    // Multiple tracks case - need to compute layout depths.
    // If no depth table name provided, create one with a constant name.
    const tableName = depthTableName ?? `__async_slice_depth_${trackIds[0]}`;

    if (depthTableName === undefined) {
      // Create the depth table if not provided by caller
      await createPerfettoTable({
        name: tableName,
        engine,
        as: `
          select
            id,
            layout_depth as depth,
            ${Math.min(...trackIds)} AS minTrackId
          from experimental_slice_layout('${trackIds.join(',')}')
        `,
      });
    }

    // Join slice with the depth table (caller-provided or self-created).
    return new SourceDataset({
      schema,
      select: {
        id: 'id',
        ts: 'ts',
        dur: 'dur',
        depth: {
          join: 'depth',
          expr: 'depth.depth',
        },
        name: 'name',
        thread_dur: 'thread_dur',
        track_id: 'track_id',
        category: 'category',
        correlation_id: "extract_arg(arg_set_id, 'correlation_id')",
        arg_set_id: 'arg_set_id',
        parent_id: 'parent_id',
      },
      src: 'slice',
      joins: {
        depth: {
          from: `${tableName} USING (id)`,
          unique: true,
        },
      },
      filter: {
        col: 'track_id',
        in: trackIds,
      },
    });
  }
}
