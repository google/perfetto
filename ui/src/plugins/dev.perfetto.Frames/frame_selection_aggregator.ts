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

import {ColumnDef, Sorting} from '../../public/aggregation';
import {Aggregation, AreaSelection} from '../../public/selection';
import {Engine} from '../../trace_processor/engine';
import {AreaSelectionAggregator} from '../../public/selection';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {
  ii,
  selectTracksAndGetDataset,
} from '../../components/aggregation_adapter';

export const ACTUAL_FRAMES_SLICE_TRACK_KIND = 'ActualFramesSliceTrack';

export class FrameSelectionAggregator implements AreaSelectionAggregator {
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
        const iiDataset = await ii(engine, this.id, dataset, area);
        await engine.query(`
          create or replace perfetto table ${this.id} as
          select
            jank_type,
            count(1) as occurrences,
            min(dur) as minDur,
            avg(dur) as meanDur,
            max(dur) as maxDur
          from (${iiDataset.query()})
          group by jank_type
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

  getDefaultSorting(): Sorting {
    return {column: 'occurrences', direction: 'DESC'};
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Jank Type',
        kind: 'STRING',
        columnConstructor: Uint16Array,
        columnId: 'jank_type',
      },
      {
        title: 'Min duration',
        kind: 'TIMESTAMP_NS',
        columnConstructor: Float64Array,
        columnId: 'minDur',
      },
      {
        title: 'Max duration',
        kind: 'TIMESTAMP_NS',
        columnConstructor: Float64Array,
        columnId: 'maxDur',
      },
      {
        title: 'Mean duration',
        kind: 'TIMESTAMP_NS',
        columnConstructor: Float64Array,
        columnId: 'meanDur',
      },
      {
        title: 'Occurrences',
        kind: 'NUMBER',
        columnConstructor: Uint16Array,
        columnId: 'occurrences',
        sum: true,
      },
    ];
  }
}
