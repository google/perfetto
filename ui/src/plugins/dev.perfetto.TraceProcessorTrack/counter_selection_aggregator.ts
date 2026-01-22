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
import {ColumnDef} from '../../components/aggregation';
import {Aggregation, Aggregator} from '../../components/aggregation_adapter';
import {AreaSelection} from '../../public/selection';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {Engine} from '../../trace_processor/engine';

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
      prepareData: async (engine: Engine) => {
        const duration = area.end - area.start;
        const durationSec = Duration.toSeconds(duration);

        await engine.query(`include perfetto module counters.intervals`);

        // TODO(lalitm): Rewrite this query in a way that is both simpler and faster
        let query;
        if (trackIds.length === 1) {
          // Optimized query for the special case where there is only 1 track id.
          query = `
            CREATE OR REPLACE PERFETTO TABLE ${this.id} AS
            WITH
              res AS (
                select c.*
                from counter_leading_intervals!((
                  SELECT counter.*
                  FROM counter
                  WHERE counter.track_id = ${trackIds[0]}
                    AND counter.ts <= ${area.end}
                )) c
                WHERE c.ts + c.dur >= ${area.start}
              ),
              aggregated AS (
                SELECT
                  COUNT(1) AS count,
                  ROUND(SUM(
                    (MIN(ts + dur, ${area.end}) - MAX(ts,${area.start}))*value)/${duration},
                    2
                  ) AS avg_value,
                  value_at_max_ts(ts, value) AS last_value,
                  value_at_max_ts(-ts, value) AS first_value,
                  MIN(value) AS min_value,
                  MAX(value) AS max_value
                FROM res
              )
            SELECT
              (SELECT name FROM counter_track WHERE id = ${trackIds[0]}) AS name,
              *,
              MAX(last_value) - MIN(first_value) AS delta_value,
              ROUND((MAX(last_value) - MIN(first_value))/${durationSec}, 2) AS rate
            FROM aggregated`;
        } else {
          // Slower, but general purspose query that can aggregate multiple tracks
          query = `
            CREATE OR REPLACE PERFETTO TABLE ${this.id} AS
            WITH
              res AS (
                select c.*
                from counter_leading_intervals!((
                  SELECT counter.*
                  FROM counter
                  WHERE counter.track_id in (${trackIds})
                    AND counter.ts <= ${area.end}
                )) c
                where c.ts + c.dur >= ${area.start}
              ),
              aggregated AS (
                SELECT track_id,
                  COUNT(1) AS count,
                  ROUND(SUM(
                    (MIN(ts + dur, ${area.end}) - MAX(ts,${area.start}))*value)/${duration},
                    2
                  ) AS avg_value,
                  value_at_max_ts(-ts, value) AS first,
                  value_at_max_ts(ts, value) AS last,
                  MIN(value) AS min_value,
                  MAX(value) AS max_value
                FROM res
                GROUP BY track_id
              )
            SELECT
              name,
              count,
              avg_value,
              last AS last_value,
              first AS first_value,
              last - first AS delta_value,
              ROUND((last - first)/${durationSec}, 2) AS rate,
              min_value,
              max_value
            FROM aggregated JOIN counter_track ON
              track_id = counter_track.id
            GROUP BY track_id`;
        }
        await engine.query(query);

        return {
          tableName: this.id,
        };
      },
    };
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Name',
        columnId: 'name',
        sort: 'DESC',
      },
      {
        title: 'Delta value',
        columnId: 'delta_value',
        formatHint: 'NUMERIC',
      },
      {
        title: 'Rate /s',
        columnId: 'rate',
        formatHint: 'NUMERIC',
      },
      {
        title: 'Weighted avg value',
        columnId: 'avg_value',
        formatHint: 'NUMERIC',
      },
      {
        title: 'Count',
        columnId: 'count',
        sum: true,
        formatHint: 'NUMERIC',
      },
      {
        title: 'First value',
        columnId: 'first_value',
        formatHint: 'NUMERIC',
      },
      {
        title: 'Last value',
        columnId: 'last_value',
        formatHint: 'NUMERIC',
      },
      {
        title: 'Min value',
        columnId: 'min_value',
        formatHint: 'NUMERIC',
      },
      {
        title: 'Max value',
        columnId: 'max_value',
        formatHint: 'NUMERIC',
      },
    ];
  }

  getTabName() {
    return 'Counters';
  }
}
