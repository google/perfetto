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
import {CPU_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM} from '../../trace_processor/query_result';

export class CpuSliceByProcessSelectionAggregator implements Aggregator {
  readonly id = 'cpu_by_process_aggregation';

  probe(area: AreaSelection): Aggregation | undefined {
    const dataset = selectTracksAndGetDataset(
      area.tracks,
      {
        id: NUM,
        dur: LONG,
        ts: LONG,
        utid: NUM,
      },
      CPU_SLICE_TRACK_KIND,
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
            process.name as process_name,
            process.pid,
            sum(dur) AS total_dur,
            sum(dur) / count() as avg_dur,
            count() as occurrences,
            cast(sum(dur) as real) / sum(sum(dur)) OVER () as percent_of_total
          from (${iiTable.name})
          join thread USING (utid)
          join process USING (upid)
          group by upid
        `);

        return {
          tableName: this.id,
        };
      },
    };
  }

  getTabName() {
    return 'CPU by process';
  }

  getDefaultSorting(): Sorting {
    return {column: 'total_dur', direction: 'DESC'};
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Process',
        columnId: 'process_name',
      },
      {
        title: 'PID',
        columnId: 'pid',
      },
      {
        title: 'Wall duration',
        formatHint: 'DURATION_NS',
        columnId: 'total_dur',
        sum: true,
      },
      {
        title: 'Wall duration %',
        formatHint: 'PERCENT',
        columnId: 'percent_of_total',
      },
      {
        title: 'Avg Wall duration',
        formatHint: 'DURATION_NS',
        columnId: 'avg_dur',
      },
      {
        title: 'Occurrences',
        columnId: 'occurrences',
        sum: true,
      },
    ];
  }
}
