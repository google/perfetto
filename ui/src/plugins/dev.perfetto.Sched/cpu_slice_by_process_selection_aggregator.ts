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
import {AreaSelection} from '../../public/selection';
import {Engine} from '../../trace_processor/engine';
import {CPU_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {AreaSelectionAggregator} from '../../public/selection';
import {Dataset} from '../../trace_processor/dataset';
import {LONG, NUM} from '../../trace_processor/query_result';

export class CpuSliceByProcessSelectionAggregator
  implements AreaSelectionAggregator
{
  readonly id = 'cpu_by_process_aggregation';
  readonly trackKind = CPU_SLICE_TRACK_KIND;
  readonly schema = {
    dur: LONG,
    ts: LONG,
    utid: NUM,
  } as const;

  async createAggregateView(
    engine: Engine,
    area: AreaSelection,
    dataset?: Dataset,
  ) {
    if (!dataset) return false;

    await engine.query(`
      create or replace perfetto table ${this.id} as
      select
        process.name as process_name,
        process.pid,
        sum(dur) AS total_dur,
        sum(dur) / count() as avg_dur,
        count() as occurrences
      from (${dataset.query()})
      join thread USING (utid)
      join process USING (upid)
      where
        ts + dur > ${area.start}
        and ts < ${area.end}
      group by upid
    `);
    return true;
  }

  getTabName() {
    return 'CPU by process';
  }

  async getExtra() {}

  getDefaultSorting(): Sorting {
    return {column: 'total_dur', direction: 'DESC'};
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
}
