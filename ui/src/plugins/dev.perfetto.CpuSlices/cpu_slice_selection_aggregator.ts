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

import {exists} from '../../base/utils';
import {ColumnDef, Sorting} from '../../public/aggregation';
import {AreaSelection} from '../../public/selection';
import {CPU_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {Engine} from '../../trace_processor/engine';
import {AreaSelectionAggregator} from '../../public/selection';

export class CpuSliceSelectionAggregator implements AreaSelectionAggregator {
  readonly id = 'cpu_aggregation';

  async createAggregateView(engine: Engine, area: AreaSelection) {
    const selectedCpus: number[] = [];
    for (const trackInfo of area.tracks) {
      if (trackInfo?.tags?.kind === CPU_SLICE_TRACK_KIND) {
        exists(trackInfo.tags.cpu) && selectedCpus.push(trackInfo.tags.cpu);
      }
    }
    if (selectedCpus.length === 0) return false;

    await engine.query(`
      create or replace perfetto table ${this.id} as
      select
        process.name as process_name,
        pid,
        thread.name as thread_name,
        tid,
        sum(dur) AS total_dur,
        sum(dur) / count() as avg_dur,
        count() as occurrences
      from process
      join thread using (upid)
      join sched using (utid)
      where cpu in (${selectedCpus})
        and sched.ts + sched.dur > ${area.start}
        and sched.ts < ${area.end}
        and utid != 0
      group by utid
    `);
    return true;
  }

  getTabName() {
    return 'CPU by thread';
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
