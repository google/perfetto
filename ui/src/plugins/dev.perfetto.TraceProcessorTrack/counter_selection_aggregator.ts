// Copyright (C) 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the \"License\");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an \"AS IS\" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {
  type Aggregation,
  type Aggregator,
  type AggregatorGridConfig,
  createAggregationData,
} from '../../components/aggregation_adapter';
import type {AreaSelection} from '../../public/selection';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import type {Engine} from '../../trace_processor/engine';
import {createPerfettoTable} from '../../trace_processor/sql_utils';

// Aggregates counter samples in the selected area. Rows are per-sample so the
// datagrid can pivot/group by name or by any arg (e.g. a `utid`), the way the
// slice aggregator pivots on `dur`. `delta` (increase since the previous
// sample on that track) is the SUM-able metric.
export class CounterSelectionAggregator implements Aggregator {
  readonly id = 'counter_aggregation';

  probe(area: AreaSelection): Aggregation | undefined {
    const trackIds: (string | number)[] = [];
    for (const trackInfo of area.tracks) {
      if (trackInfo?.tags?.kinds?.includes(COUNTER_TRACK_KIND)) {
        trackInfo.tags?.trackIds && trackIds.push(...trackInfo.tags.trackIds);
      }
    }
    if (trackIds.length === 0) return undefined;

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        const table = await createPerfettoTable({
          engine,
          as: `
            WITH samples AS (
              SELECT
                c.ts,
                c.value,
                c.track_id,
                c.arg_set_id,
                c.value - LAG(c.value) OVER (
                  PARTITION BY c.track_id ORDER BY c.ts
                ) AS delta
              FROM counter c
              WHERE c.track_id IN (${trackIds})
                AND c.ts <= ${area.end}
            )
            SELECT
              ct.name AS name,
              s.ts AS ts,
              s.value AS value,
              s.delta AS delta,
              s.arg_set_id AS arg_set_id
            FROM samples s
            JOIN counter_track ct ON ct.id = s.track_id
            WHERE s.ts >= ${area.start}
          `,
        });
        return createAggregationData(table);
      },
    };
  }

  private getGridConfig(): AggregatorGridConfig {
    return {
      schema: {
        name: {title: 'Name', columnType: 'text'},
        ts: {title: 'Timestamp', columnType: 'quantitative'},
        value: {title: 'Value', columnType: 'quantitative'},
        delta: {title: 'Increase', columnType: 'quantitative'},
        args: {title: 'Args', parameterized: true},
      },
      // The table has an `arg_set_id` column, so expose a parameterized
      // `args.*` column, letting the datagrid group/pivot by any counter arg.
      sqlConfig: ({sqlTable}) => ({
        tableOrSubquery: sqlTable.get().name,
        columns: {
          args: {
            expression: (alias, key) =>
              `extract_arg(${alias}.arg_set_id, '${key}')`,
            parameterized: true,
            parameterKeysQuery: (tableOrSubquery, alias) => `
              SELECT DISTINCT args.key
              FROM (${tableOrSubquery}) AS ${alias}
              JOIN args ON args.arg_set_id = ${alias}.arg_set_id
              WHERE args.key IS NOT NULL
              ORDER BY args.key
              LIMIT 1000
            `,
          },
        },
      }),
      initialColumns: [
        {id: 'name', field: 'name'},
        {id: 'ts', field: 'ts'},
        {id: 'value', field: 'value'},
        {id: 'delta', field: 'delta'},
      ],
      initialPivot: {
        groupBy: [{id: 'name', field: 'name'}],
        aggregates: [
          {id: 'count', function: 'COUNT'},
          {id: 'delta_sum', field: 'delta', function: 'SUM', sort: 'DESC'},
        ],
      },
    };
  }

  getTabName() {
    return 'Counters';
  }
}
