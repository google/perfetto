// Copyright (C) 2021 The Android Open Source Project
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

import {
  AggregatePivotModel,
  Aggregation,
  Aggregator,
  createIITable,
  selectTracksAndGetDataset,
} from '../../components/aggregation_adapter';
import {AreaSelection} from '../../public/selection';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM, STR} from '../../trace_processor/query_result';

export const ACTUAL_FRAMES_SLICE_TRACK_KIND = 'ActualFramesSliceTrack';

export class FrameSelectionAggregator implements Aggregator {
  readonly id = 'frame_aggregation';

  probe(area: AreaSelection): Aggregation | undefined {
    const dataset = selectTracksAndGetDataset(
      area.tracks,
      {
        id: NUM,
        ts: LONG,
        dur: LONG,
        jank_type: STR,
      },
      ACTUAL_FRAMES_SLICE_TRACK_KIND,
    );

    if (!dataset) return undefined;

    return {
      prepareData: async (engine: Engine) => {
        await using iiTable = await createIITable(
          engine,
          dataset,
          area.start,
          area.end,
        );
        await engine.query(`
          create or replace perfetto table ${this.id} as
          select
            jank_type,
            dur
          from (${iiTable.name})
        `);

        return {
          tableName: this.id,
        };
      },
    };
  }

  getTabName() {
    return 'Frames';
  }

  getColumnDefinitions(): AggregatePivotModel {
    return {
      groupBy: [{field: 'jank_type'}],
      aggregates: [
        {
          function: 'COUNT',
          sort: 'DESC',
        },
        {
          field: 'dur',
          function: 'MIN',
        },
        {
          field: 'dur',
          function: 'MAX',
        },
        {
          field: 'dur',
          function: 'AVG',
        },
      ],
      columns: [
        {
          title: 'Jank Type',
          columnId: 'jank_type',
        },
        {
          title: 'Duration',
          formatHint: 'DURATION_NS',
          columnId: 'dur',
        },
      ],
    };
  }
}
