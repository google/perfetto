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

import {ColumnDef, Sorting, BarChartData} from '../../public/aggregation';
import {AreaSelection} from '../../public/selection';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM, STR, STR_NULL} from '../../trace_processor/query_result';
import {AreaSelectionAggregator} from '../../public/selection';
import {Dataset} from '../../trace_processor/dataset';
import {colorForThreadState} from './common';

export class ThreadStateSelectionAggregator implements AreaSelectionAggregator {
  readonly id = 'thread_state_aggregation';

  readonly schema = {
    dur: LONG,
    state: STR,
    utid: NUM,
  } as const;

  async createAggregateView(
    engine: Engine,
    _area: AreaSelection,
    dataset?: Dataset,
  ) {
    if (dataset === undefined) return false;

    await engine.query(`
      create or replace perfetto table ${this.id} as
      select
        process.name as process_name,
        process.pid,
        thread.name as thread_name,
        thread.tid,
        tstate.state as state,
        sum(tstate.dur) AS total_dur,
        sum(tstate.dur) / count() as avg_dur,
        count() as occurrences
      from (${dataset.query()}) tstate
      join thread using (utid)
      left join process using (upid)
      group by utid, state
    `);

    return true;
  }

  async getBarChartData(
    engine: Engine,
    _area: AreaSelection,
    dataset?: Dataset,
  ): Promise<BarChartData[] | undefined> {
    if (dataset === undefined) return undefined;

    const query = `
      select
        tstate.state as state,
        sum(dur) as totalDur
      from (${dataset.query()}) tstate
      join thread using (utid)
      group by tstate.state
    `;
    const result = await engine.query(query);

    const it = result.iter({
      state: STR_NULL,
      totalDur: NUM,
    });

    const states: BarChartData[] = [];
    for (let i = 0; it.valid(); ++i, it.next()) {
      const name = it.state ?? 'Unknown';
      const ms = it.totalDur / 1000000;
      states.push({
        name,
        timeInStateMs: ms,
        color: colorForThreadState(name),
      });
    }
    return states;
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Process',
        kind: 'STRING',
        columnConstructor: Uint16Array,
        columnId: 'process_name',
      },
      {
        title: 'PID',
        kind: 'NUMBER',
        columnConstructor: Uint16Array,
        columnId: 'pid',
      },
      {
        title: 'Thread',
        kind: 'STRING',
        columnConstructor: Uint16Array,
        columnId: 'thread_name',
      },
      {
        title: 'TID',
        kind: 'NUMBER',
        columnConstructor: Uint16Array,
        columnId: 'tid',
      },
      {
        title: 'State',
        kind: 'STRING',
        columnConstructor: Uint16Array,
        columnId: 'state',
      },
      {
        title: 'Wall duration (ms)',
        kind: 'TIMESTAMP_NS',
        columnConstructor: Float64Array,
        columnId: 'total_dur',
        sum: true,
      },
      {
        title: 'Avg Wall duration (ms)',
        kind: 'TIMESTAMP_NS',
        columnConstructor: Float64Array,
        columnId: 'avg_dur',
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

  getTabName() {
    return 'Thread States';
  }

  getDefaultSorting(): Sorting {
    return {column: 'total_dur', direction: 'DESC'};
  }
}
