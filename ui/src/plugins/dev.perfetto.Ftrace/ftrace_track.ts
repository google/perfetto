// Copyright (C) 2025 The Android Open Source Project
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

import {Store} from '../../base/store';
import {materialColorScheme} from '../../components/colorizer';
import {SliceTrack} from '../../components/tracks/slice_track';
import {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {FtraceFilter} from './common';
import {FtraceEventDetailsPanel} from './ftrace_details_panel';

const FTRACE_INSTANT_WIDTH_PX = 8;

export function createFtraceTrack(
  trace: Trace,
  uri: string,
  ucpu: number,
  store: Store<FtraceFilter>,
) {
  return SliceTrack.create({
    trace,
    uri,
    dataset: () => {
      // This dataset can change depending on the filter settings, so we pass a
      // function in here instead of a static dataset. This function is called
      // every render cycle by the track to see if the dataset has changed.
      const excludeList = store.state.excludeList;
      return new SourceDataset({
        src: `
          SELECT *
          FROM ftrace_event
          WHERE
            name NOT IN (${excludeList.map((x) => `'${x}'`).join(', ')})
        `,
        schema: {
          id: NUM,
          ts: LONG,
          name: STR,
          cpu: NUM,
        },
        filter: {
          col: 'ucpu',
          eq: ucpu,
        },
      });
    },
    colorizer: (row) => materialColorScheme(row.name),
    instantStyle: {
      width: FTRACE_INSTANT_WIDTH_PX,
      render: (ctx, r) => ctx.fillRect(r.x, r.y, r.width, r.height),
    },
    forceTsRenderOrder: true,
    tooltip: (row) => row.row.name,
    detailsPanel: (row) => {
      return new FtraceEventDetailsPanel(trace, row);
    },
  });
}
