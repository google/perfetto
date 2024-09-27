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

import {ColumnDef, Sorting} from '../../public/aggregation';
import {AreaSelection} from '../../public/selection';
import {Engine} from '../../trace_processor/engine';
import {AreaSelectionAggregator} from '../../public/selection';
import {
  ASYNC_SLICE_TRACK_KIND,
  THREAD_SLICE_TRACK_KIND,
} from '../../public/track_kinds';

export class AsyncAndThreadSliceSelectionAggregator
  implements AreaSelectionAggregator
{
  readonly id = 'slice_aggregation';

  async createAggregateView(engine: Engine, area: AreaSelection) {
    const selectedTrackKeys = getSelectedTrackSqlIds(area);

    if (selectedTrackKeys.length === 0) return false;

    await engine.query(`
      create or replace perfetto table ${this.id} as
      select
        name,
        sum(dur) AS total_dur,
        sum(dur)/count() as avg_dur,
        count() as occurrences
        from slices
      where track_id in (${selectedTrackKeys})
        and ts + dur > ${area.start}
        and ts < ${area.end}
      group by name
    `);
    return true;
  }

  getTabName() {
    return 'Slices';
  }

  async getExtra() {}

  getDefaultSorting(): Sorting {
    return {column: 'total_dur', direction: 'DESC'};
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Name',
        kind: 'STRING',
        columnConstructor: Uint32Array,
        columnId: 'name',
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
        columnConstructor: Uint32Array,
        columnId: 'occurrences',
        sum: true,
      },
    ];
  }
}

function getSelectedTrackSqlIds(area: AreaSelection): number[] {
  const selectedTrackKeys: number[] = [];
  for (const trackInfo of area.tracks) {
    if (trackInfo?.tags?.kind === THREAD_SLICE_TRACK_KIND) {
      trackInfo.tags.trackIds &&
        selectedTrackKeys.push(...trackInfo.tags.trackIds);
    }
    if (trackInfo?.tags?.kind === ASYNC_SLICE_TRACK_KIND) {
      trackInfo.tags.trackIds &&
        selectedTrackKeys.push(...trackInfo.tags.trackIds);
    }
  }
  return selectedTrackKeys;
}
