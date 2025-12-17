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

import {Duration} from '../../base/time';
import {BarChartData} from '../../components/aggregation';
import {
  AggregatePivotModel,
  Aggregation,
  Aggregator,
  createIITable,
  selectTracksAndGetDataset,
} from '../../components/aggregation_adapter';
import {AreaSelection} from '../../public/selection';
import {THREAD_STATE_TRACK_KIND} from '../../public/track_kinds';
import {Engine} from '../../trace_processor/engine';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {colorForThreadState} from './common';

export class ThreadStateByCpuAggregator implements Aggregator {
  readonly id = 'thread_state_by_cpu_aggregation';

  probe(area: AreaSelection): Aggregation | undefined {
    const dataset = selectTracksAndGetDataset(
      area.tracks,
      {
        id: NUM,
        ts: LONG,
        dur: LONG,
        ucpu: NUM_NULL,
        state: STR,
        utid: NUM,
      },
      THREAD_STATE_TRACK_KIND,
    );

    // If we couldn't pick out a dataset, we have nothing to show for this
    // selection so just return undefined to indicate that no tab should be
    // displayed.
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
            thread.name as thread_name,
            thread.tid,
            tstate.state as state,
            utid,
            ucpu,
            dur,
            dur * 1.0 / sum(dur) OVER () as fraction_of_total
          from (${iiTable.name}) tstate
          join thread using (utid)
          left join process using (upid)
        `);

        const query = `
          select
            tstate.state as state,
            sum(dur) as totalDur
          from (${iiTable.name}) tstate
          join thread using (utid)
          group by tstate.state
        `;
        const result = await engine.query(query);

        const it = result.iter({
          state: STR_NULL,
          totalDur: LONG,
        });

        const states: BarChartData[] = [];
        for (let i = 0; it.valid(); ++i, it.next()) {
          const name = it.state ?? 'Unknown';
          states.push({
            title: `${name}: ${Duration.humanise(it.totalDur)}`,
            value: Number(it.totalDur),
            color: colorForThreadState(name),
          });
        }

        return {
          tableName: this.id,
          barChartData: states,
        };
      },
    };
  }

  getColumnDefinitions(): AggregatePivotModel {
    return {
      groupBy: [{field: 'utid'}, {field: 'state'}, {field: 'ucpu'}],
      aggregates: [
        {function: 'COUNT'},
        {field: 'process_name', function: 'ANY'},
        {field: 'pid', function: 'ANY'},
        {field: 'thread_name', function: 'ANY'},
        {field: 'tid', function: 'ANY'},
        {field: 'dur', function: 'SUM', sort: 'DESC'},
        {field: 'fraction_of_total', function: 'SUM'},
        {field: 'dur', function: 'AVG'},
      ],
      columns: [
        {
          title: 'Process',
          columnId: 'process_name',
        },
        {
          title: 'PID',
          columnId: 'pid',
          formatHint: 'NUMERIC',
        },
        {
          title: 'Thread',
          columnId: 'thread_name',
        },
        {
          title: 'TID',
          columnId: 'tid',
          formatHint: 'NUMERIC',
        },
        {
          title: 'CPU',
          columnId: 'ucpu',
          formatHint: 'NUMERIC',
        },
        {
          title: 'UTID',
          columnId: 'utid',
          formatHint: 'NUMERIC',
        },
        {
          title: 'State',
          columnId: 'state',
        },
        {
          title: 'Wall duration',
          formatHint: 'DURATION_NS',
          columnId: 'dur',
        },
        {
          title: 'Wall duration %',
          formatHint: 'PERCENT',
          columnId: 'fraction_of_total',
        },
      ],
    };
  }

  getTabName() {
    return 'Thread States by CPU';
  }
}
