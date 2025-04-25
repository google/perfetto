// Copyright (C) 2025 The Android Open Source Project
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
import {ColumnDef, Sorting} from '../../public/aggregation';
import {AreaSelection} from '../../public/selection';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {Engine} from '../../trace_processor/engine';
import {AreaSelectionAggregator} from '../../public/selection';
import {LONG, NUM} from '../../trace_processor/query_result';

export class PowerCounterSelectionAggregator
  implements AreaSelectionAggregator
{
  readonly id = 'power_counter_aggregation';

  // This just describes which counters we match, we don't actually use the
  // resulting datasets, but it's a useful too to show what we actually match.
  readonly trackKind = COUNTER_TRACK_KIND;
  readonly schema = {
    id: NUM,
    ts: LONG,
    value: NUM,
  };

  async createAggregateView(engine: Engine, area: AreaSelection) {
    const trackIds: (string | number)[] = [];
    for (const trackInfo of area.tracks) {
      if (
        trackInfo?.tags?.kind === COUNTER_TRACK_KIND &&
        trackInfo?.tags?.type === 'power_rails'
      ) {
        trackInfo.tags?.trackIds && trackIds.push(...trackInfo.tags.trackIds);
      }
    }
    if (trackIds.length === 0) return false;
    const duration = area.end - area.start;
    const durationSec = Duration.toSeconds(duration);

    const query = `CREATE OR REPLACE PERFETTO TABLE ${this.id} AS
      WITH  aggregated AS (
        SELECT track_id,
          COUNT(1) AS count,
          value_at_max_ts(-ts, value) AS first,
          value_at_max_ts(ts, value) AS last
        FROM counter
        WHERE counter.track_id in (${trackIds})
          AND ts BETWEEN ${area.start} AND ${area.end}
        GROUP BY track_id
      )
      SELECT
        name,
        count,
        last - first AS delta_value,
        ROUND((last - first)/${durationSec}, 2) AS rate
      FROM aggregated JOIN counter_track ON
        track_id = counter_track.id
      GROUP BY track_id`;
    await engine.query(query);
    return true;
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Rail Name',
        kind: 'STRING',
        columnConstructor: Uint16Array,
        columnId: 'name',
      },
      {
        title: 'Delta energy (uJ)',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'delta_value',
      },
      {
        title: 'Avg Power (uW)',
        kind: 'Number',
        columnConstructor: Float64Array,
        columnId: 'rate',
      },
      {
        title: 'Sample Count',
        kind: 'Number',
        columnConstructor: Float64Array,
        columnId: 'count',
        sum: true,
      },
    ];
  }

  async getExtra() {}

  getTabName() {
    return 'Power Counters';
  }

  getDefaultSorting(): Sorting {
    return {column: 'name', direction: 'DESC'};
  }
}
