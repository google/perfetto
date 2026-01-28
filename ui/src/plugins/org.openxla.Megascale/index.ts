// Copyright (C) 2024 The Android Open Source Project
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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {addQueryResultsTab} from '../../components/query_table/query_result_tab';

export default class MegascalePlugin implements PerfettoPlugin {
  static readonly id = 'org.openxla.Megascale';

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.onTraceReady.addListener(() => {
      addQueryResultsTab(trace, {
        query: `
          SELECT
            a.int_value AS network_latency_us,
            s.slice_id,
            s.id,
            s.name,
            s.ts,
            s.dur,
            s.track_id,
            b.string_value AS buffer_sizes
          FROM slice AS s
          JOIN
            args AS a
            ON s.arg_set_id = a.arg_set_id
          JOIN
            args AS b
            ON s.arg_set_id = b.arg_set_id
          WHERE
            s.track_id NOT IN (
              SELECT id FROM track WHERE name LIKE '%XLA Ops%'
            )
            AND s.name = 'NetworkReceive END'
            AND a.key = 'debug.network_transport_latency_us'
            AND b.key = 'debug.buffer_sizes';
        `,
        title: 'recv latency',
      });

      addQueryResultsTab(trace, {
        query: `
          SELECT
            a.int_value / 1000 AS network_latency_us,
            s.slice_id,
            s.id,
            s.name,
            s.ts,
            s.dur,
            s.track_id,
            b.string_value AS buffer_sizes
          FROM slice AS s
          JOIN
            args AS a
            ON s.arg_set_id = a.arg_set_id
          JOIN
            args AS b
            ON s.arg_set_id = b.arg_set_id
          WHERE
            s.track_id NOT IN (
              SELECT id FROM track WHERE name LIKE '%XLA Ops%'
            )
            AND s.name = 'NetworkSend END'
            AND a.key = 'debug.action_duration_ns'
            AND b.key = 'debug.buffer_sizes';
        `,
        title: 'send latency',
      });

      addQueryResultsTab(trace, {
        query: `
          SELECT
            slice.slice_id,
            slice.id,
            slice.name,
            slice.ts,
            slice.dur,
            slice.track_id,
            parent_track.name AS parent_track_name
          FROM slice
          JOIN track AS child_track
          ON slice.track_id = child_track.id
          LEFT JOIN track AS parent_track
          ON child_track.parent_id = parent_track.id
          WHERE child_track.name LIKE '%XLA Ops%' AND slice.name REGEXP '^recv-done.[0-9]+$'
          ORDER BY slice.dur DESC;
        `,
        title: 'all recv-done ops',
      });

      addQueryResultsTab(trace, {
        query: `
          SELECT
            s.name,
            ROUND(PERCENTILE(s.dur, 50), 2) AS dur_ns_p50,
            ROUND(PERCENTILE(s.dur, 90), 2) AS dur_ns_p90,
            ROUND(PERCENTILE(s.dur, 99), 2) AS dur_ns_p99,
            ROUND(AVG(s.dur), 2) AS dur_ns_mean,
            COUNT(*) AS count,
            SUM(s.dur) AS dur_ns_sum,
            ROUND(PERCENTILE(s.dur, 99) / AVG(s.dur), 2) AS p99_over_mean
          FROM slice AS s
          WHERE
            s.name REGEXP '^recv-done.[0-9]+$'
            AND s.track_id IN (
              SELECT id FROM track WHERE name LIKE '%XLA Ops%'
            )
          GROUP BY s.name
          ORDER BY s.name;
        `,
        title: 'recv-done stats',
      });
    });
  }
}
