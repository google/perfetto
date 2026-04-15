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
import {
  Aggregation,
  Aggregator,
  AggregatorGridConfig,
} from '../../components/aggregation_adapter';
import {AreaSelection} from '../../public/selection';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM} from '../../trace_processor/query_result';

export class PowerCounterSelectionAggregator implements Aggregator {
  readonly id = 'power_counter_aggregation';

  // This just describes which counters we match, we don't actually use the
  // resulting datasets, but it's a useful too to show what we actually match.
  readonly trackKind = COUNTER_TRACK_KIND;
  readonly schema = {
    id: NUM,
    ts: LONG,
    value: NUM,
  };

  probe(area: AreaSelection): Aggregation | undefined {
    const trackIds: (string | number)[] = [];
    for (const trackInfo of area.tracks) {
      if (
        trackInfo?.tags?.kinds?.includes(COUNTER_TRACK_KIND) &&
        trackInfo?.tags?.type === 'power_rails'
      ) {
        trackInfo.tags?.trackIds && trackIds.push(...trackInfo.tags.trackIds);
      }
    }
    if (trackIds.length === 0) return undefined;

    return {
      prepareData: async (engine: Engine) => {
        const duration = area.end - area.start;
        const durationSec = Duration.toSeconds(duration);

        const query = `INCLUDE PERFETTO MODULE android.power_rails;
          CREATE OR REPLACE PERFETTO TABLE ${this.id} AS
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
            COALESCE(friendly_name, raw_power_rail_name) AS name,
            count,
            (last - first) / 1000 AS delta_value,
            ROUND((last - first)/${durationSec} / 1000, 2) AS rate
          FROM aggregated JOIN android_power_rails_metadata USING (track_id)
          GROUP BY track_id
        `;
        await engine.query(query);

        return {
          tableName: this.id,
        };
      },
    };
  }

  getGridConfig(): AggregatorGridConfig {
    return {
      schema: {
        name: {title: 'Rail Name', columnType: 'text'},
        delta_value: {title: 'Delta energy (mJ)', columnType: 'quantitative'},
        rate: {title: 'Avg Power (mW)', columnType: 'quantitative'},
        count: {title: 'Sample Count', columnType: 'quantitative'},
      },
      initialColumns: [
        {id: 'name', field: 'name', sort: 'DESC'},
        {id: 'delta_value', field: 'delta_value', aggregate: 'SUM'},
        {id: 'rate', field: 'rate', aggregate: 'SUM'},
        {id: 'count', field: 'count', aggregate: 'SUM'},
      ],
    };
  }

  getTabName() {
    return 'Power Counters';
  }
}
