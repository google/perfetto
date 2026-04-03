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
  Aggregator,
  AggregatorGridConfig,
} from '../../components/aggregation_adapter';
import {AreaSelection} from '../../public/selection';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {Engine} from '../../trace_processor/engine';
import {formatPercentValue} from '../../components/aggregation_panel';

export class EntityStateResidencySelectionAggregator implements Aggregator {
  readonly id = 'entity_state_residency_aggregation';

  probe(area: AreaSelection) {
    const trackIds: (string | number)[] = [];
    for (const trackInfo of area.tracks) {
      if (
        trackInfo?.tags?.kinds?.includes(COUNTER_TRACK_KIND) &&
        trackInfo?.tags?.type === 'entity_state'
      ) {
        trackInfo.tags?.trackIds && trackIds.push(...trackInfo.tags.trackIds);
      }
    }

    if (trackIds.length === 0) {
      return undefined;
    }

    return {
      prepareData: async (engine: Engine) => {
        const duration = area.end - area.start;
        const durationSec = Duration.toSeconds(duration);

        const query = `
          INCLUDE PERFETTO MODULE android.entity_state_residency;
          CREATE OR REPLACE PERFETTO TABLE ${this.id} AS
            WITH aggregated AS (
              SELECT
                track_id,
                entity_name,
                state_name,
                COUNT(state_time_since_boot) AS count,
                value_at_max_ts(-ts, state_time_since_boot) AS first,
                value_at_max_ts(ts, state_time_since_boot) AS last
              FROM android_entity_state_residency
              WHERE track_id in (${trackIds})
                AND ts BETWEEN ${area.start} AND ${area.end}
              GROUP BY track_id, entity_name, state_name
            )
            SELECT
              entity_name,
              state_name,
              count,
              (last - first) / 1e6 AS delta_value,
              (last - first)/(${durationSec} * 1e9) AS rate_percent
            FROM aggregated
            GROUP BY track_id, entity_name, state_name
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
        entity_name: {title: 'Entity', columnType: 'text'},
        state_name: {title: 'State', columnType: 'text'},
        delta_value: {title: 'Time in state (ms)', columnType: 'quantitative'},
        rate_percent: {
          title: 'Time in state',
          columnType: 'quantitative',
          cellRenderer: formatPercentValue,
        },
        count: {title: 'Sample Count', columnType: 'quantitative'},
      },
      initialColumns: [
        {id: 'entity_name', field: 'entity_name', sort: 'DESC'},
        {id: 'state_name', field: 'state_name'},
        {id: 'delta_value', field: 'delta_value', aggregate: 'SUM'},
        {id: 'rate_percent', field: 'rate_percent', aggregate: 'SUM'},
        {id: 'count', field: 'count'},
      ],
    };
  }

  getTabName() {
    return 'Entity State Residency';
  }
}
