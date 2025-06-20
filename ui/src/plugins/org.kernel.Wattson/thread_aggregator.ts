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

import {
  Aggregation,
  AreaSelection,
  AreaSelectionAggregator,
} from '../../public/selection';
import {ColumnDef, Sorting} from '../../public/aggregation';
import {CPU_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {Engine} from '../../trace_processor/engine';
import {exists} from '../../base/utils';

export class WattsonThreadSelectionAggregator
  implements AreaSelectionAggregator
{
  readonly id = 'wattson_plugin_thread_aggregation';

  probe(area: AreaSelection): Aggregation | undefined {
    const selectedCpus: number[] = [];
    for (const trackInfo of area.tracks) {
      if (trackInfo?.tags?.kind === CPU_SLICE_TRACK_KIND) {
        exists(trackInfo.tags.cpu) && selectedCpus.push(trackInfo.tags.cpu);
      }
    }
    if (selectedCpus.length === 0) return undefined;

    return {
      prepareData: async (engine: Engine) => {
        await engine.query(`drop view if exists ${this.id};`);
        const duration = area.end - area.start;
        const cpusCsv = `(` + selectedCpus.join() + `)`;
        engine.query(`
          INCLUDE PERFETTO MODULE wattson.tasks.attribution;
          INCLUDE PERFETTO MODULE wattson.tasks.idle_transitions_attribution;
          INCLUDE PERFETTO MODULE wattson.ui.continuous_estimates;

          CREATE OR REPLACE PERFETTO TABLE wattson_plugin_ui_selection_window AS
          SELECT
            ${area.start} as ts,
            ${duration} as dur;

          -- Processes filtered by CPU within the UI defined time window
          DROP TABLE IF EXISTS wattson_plugin_windowed_summary;
          CREATE VIRTUAL TABLE wattson_plugin_windowed_summary
          USING SPAN_JOIN(
            wattson_plugin_ui_selection_window,
            _estimates_w_tasks_attribution
          );

          -- Only get idle attribution in user defined window and filter by selected
          -- CPUs
          CREATE OR REPLACE PERFETTO TABLE wattson_plugin_idle_attribution AS
          SELECT
            idle_cost_mws,
            utid,
            upid
          FROM _filter_idle_attribution(${area.start}, ${duration})
          WHERE cpu in ${cpusCsv};

          -- Group idle attribution by thread
          CREATE OR REPLACE PERFETTO TABLE wattson_plugin_per_thread_idle_cost AS
          SELECT
            SUM(idle_cost_mws) as idle_cost_mws,
            utid
          FROM wattson_plugin_idle_attribution
          GROUP BY utid;

          CREATE OR REPLACE PERFETTO TABLE wattson_plugin_unioned_per_cpu_total AS
          SELECT
            SUM(estimated_mw * dur) AS total_pws,
            SUM(dur) AS dur,
            tid,
            pid,
            uid,
            utid,
            upid,
            thread_name,
            process_name,
            package_name
          FROM wattson_plugin_windowed_summary
          WHERE cpu in ${cpusCsv}
          GROUP BY utid;

          -- Grouped again by UTID, but this time to make it CPU agnostic
          CREATE PERFETTO VIEW ${this.id} AS
          WITH base AS (
            SELECT
              ROUND(SUM(total_pws) / ${duration}, 3) as active_mw,
              ROUND(SUM(total_pws) / 1000000000, 3) as active_mws,
              ROUND(COALESCE(idle_cost_mws, 0), 3) as idle_cost_mws,
              ROUND(
                COALESCE(idle_cost_mws, 0) + SUM(total_pws) / 1000000000,
                3
              ) as total_mws,
              thread_name,
              utid,
              tid,
              pid
            FROM wattson_plugin_unioned_per_cpu_total
            LEFT JOIN wattson_plugin_per_thread_idle_cost USING (utid)
            GROUP BY utid
          ),
          secondary AS (
            SELECT utid,
              ROUND(100 * (total_mws) / (SUM(total_mws) OVER()), 3)
                AS percent_of_total_energy
            FROM base
            GROUP BY utid
          )
          select *
            from base INNER JOIN secondary
            USING (utid);
        `);

        return {
          tableName: this.id,
        };
      },
    };
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Thread Name',
        kind: 'STRING',
        columnConstructor: Uint16Array,
        columnId: 'thread_name',
      },
      {
        title: 'TID',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'tid',
      },
      {
        title: 'PID',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'pid',
      },
      {
        title: 'Active power (estimated mW)',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'active_mw',
        sum: true,
      },
      {
        title: 'Active energy (estimated mWs)',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'active_mws',
        sum: true,
      },
      {
        title: 'Idle transitions overhead (estimated mWs)',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'idle_cost_mws',
        sum: false,
      },
      {
        title: 'Total energy (estimated mWs)',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'total_mws',
        sum: true,
      },
      {
        title: '% of total energy',
        kind: 'PERCENT',
        columnConstructor: Float64Array,
        columnId: 'percent_of_total_energy',
        sum: false,
      },
    ];
  }

  getTabName() {
    return 'Wattson by thread';
  }

  getDefaultSorting(): Sorting {
    return {column: 'active_mws', direction: 'DESC'};
  }
}
