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

import {ColumnDef} from '../../common/aggregation_data';
import {Engine} from '../../common/engine';
import {Area, Sorting} from '../../common/state';
import {toNs} from '../../common/time';
import {Config, COUNTER_TRACK_KIND} from '../../tracks/counter/common';
import {globals} from '../globals';

import {AggregationController} from './aggregation_controller';

export class CounterAggregationController extends AggregationController {
  async createAggregateView(engine: Engine, area: Area) {
    await engine.query(`drop view if exists ${this.kind};`);

    const ids = [];
    for (const trackId of area.tracks) {
      const track = globals.state.tracks[trackId];
      // Track will be undefined for track groups.
      if (track !== undefined && track.kind === COUNTER_TRACK_KIND) {
        const config = track.config as Config;
        // TODO(taylori): Also aggregate annotation (with namespace) counters.
        if (config.namespace === undefined) {
          ids.push(config.trackId);
        }
      }
    }
    if (ids.length === 0) return false;

    const query = `create view ${this.kind} as select
    name,
    count(1) as count,
    round(sum(weighted_value)/${
        toNs(area.endSec) - toNs(area.startSec)}, 2) as avg_value,
    last as last_value,
    first as first_value,
    max(last) - min(first) as delta_value,
    round((max(last) - min(first))/${area.endSec - area.startSec}, 2) as rate,
    min(value) as min_value,
    max(value) as max_value
    from
        (select *,
        (min(ts + dur, ${toNs(area.endSec)}) - max(ts,${toNs(area.startSec)}))
        * value as weighted_value,
        first_value(value) over
        (partition by track_id order by ts) as first,
        last_value(value) over
        (partition by track_id order by ts
            range between unbounded preceding and unbounded following) as last
        from experimental_counter_dur
        where track_id in (${ids})
        and ts + dur >= ${toNs(area.startSec)} and
        ts <= ${toNs(area.endSec)})
    join counter_track
    on track_id = counter_track.id
    group by track_id`;

    await engine.query(query);
    return true;
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Name',
        kind: 'STRING',
        columnConstructor: Uint16Array,
        columnId: 'name',
      },
      {
        title: 'Delta value',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'delta_value'
      },
      {
        title: 'Rate /s',
        kind: 'Number',
        columnConstructor: Float64Array,
        columnId: 'rate'
      },
      {
        title: 'Weighted avg value',
        kind: 'Number',
        columnConstructor: Float64Array,
        columnId: 'avg_value'
      },
      {
        title: 'Count',
        kind: 'Number',
        columnConstructor: Float64Array,
        columnId: 'count'
      },
      {
        title: 'First value',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'first_value'
      },
      {
        title: 'Last value',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'last_value'
      },
      {
        title: 'Min value',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'min_value'
      },
      {
        title: 'Max value',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'max_value'
      },

    ];
  }

  async getExtra() {}

  getTabName() {
    return 'Counters';
  }

  getDefaultSorting(): Sorting {
    return {column: 'name', direction: 'DESC'};
  }
}
