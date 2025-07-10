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

import {ColumnDef, Sorting} from '../../components/aggregation';
import {
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
            count(1) as occurrences,
            min(dur) as minDur,
            avg(dur) as meanDur,
            max(dur) as maxDur
          from (${iiTable.name})
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
        columnId: 'jank_type',
      },
      {
        title: 'Min duration',
        formatHint: 'DURATION_NS',
        columnId: 'minDur',
      },
      {
        title: 'Max duration',
        formatHint: 'DURATION_NS',
        columnId: 'maxDur',
      },
      {
        title: 'Mean duration',
        formatHint: 'DURATION_NS',
        columnId: 'meanDur',
      },
      {
        title: 'Occurrences',
        columnId: 'occurrences',
        sum: true,
      },
    ];
  }
}
