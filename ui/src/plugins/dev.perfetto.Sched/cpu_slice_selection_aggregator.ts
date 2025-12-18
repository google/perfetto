// Copyright (C) 2020 The Android Open Source Project
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

import {Sorting} from '../../components/aggregation';
import {
  AggregatePivotModel,
  Aggregation,
  Aggregator,
  createIITable,
  selectTracksAndGetDataset,
} from '../../components/aggregation_adapter';
import {AreaSelection} from '../../public/selection';
import {CPU_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM} from '../../trace_processor/query_result';

export class CpuSliceSelectionAggregator implements Aggregator {
  readonly id = 'cpu_aggregation';

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
            utid,
            process.name as process_name,
            pid,
            thread.name as thread_name,
            tid,
            dur,
            dur * 1.0 / sum(dur) OVER () as fraction_of_total,
            dur * 1.0 / ${area.end - area.start} as fraction_of_selection
          from (${iiTable.name}) as sched
          join thread using (utid)
          left join process using (upid)
        `);

        return {
          tableName: this.id,
        };
      },
    };
  }

  getTabName() {
    return 'CPU by thread';
  }

  getDefaultSorting(): Sorting {
    return {column: 'total_dur', direction: 'DESC'};
  }

  getColumnDefinitions(): AggregatePivotModel {
    return {
      groupBy: ['pid', 'process_name', 'tid', 'thread_name'],
      values: {
        count: {func: 'COUNT'},
        total_dur: {col: 'dur', func: 'SUM'},
        fraction_of_total: {col: 'fraction_of_total', func: 'SUM'},
        avg_dur: {col: 'dur', func: 'AVG'},
      },
      columns: [
        {
          title: 'PID',
          columnId: 'pid',
          formatHint: 'NUMERIC',
        },
        {
          title: 'Process Name',
          columnId: 'process_name',
          formatHint: 'STRING',
        },
        {
          title: 'TID',
          columnId: 'tid',
          formatHint: 'NUMERIC',
        },
        {
          title: 'Thread Name',
          columnId: 'thread_name',
          formatHint: 'STRING',
        },
        {
          title: 'Wall Duration',
          formatHint: 'DURATION_NS',
          columnId: 'dur',
        },
        {
          title: 'Wall Duration as % of Total',
          columnId: 'fraction_of_total',
          formatHint: 'PERCENT',
        },
        {
          title: 'Wall Duration % of Selection',
          columnId: 'fraction_of_selection',
          formatHint: 'PERCENT',
        },
      ],
    };
  }
}
