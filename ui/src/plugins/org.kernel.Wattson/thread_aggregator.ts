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

import m from 'mithril';
import {exists} from '../../base/utils';
import {addWattsonThreadTrack} from './wattson_thread_utils';
import {
  Aggregation,
  Aggregator,
  AggregatorGridConfig,
} from '../../components/aggregation_adapter';
import {AreaSelection} from '../../public/selection';
import {Button, ButtonVariant} from '../../widgets/button';
import {CPU_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {Engine} from '../../trace_processor/engine';
import {Intent} from '../../widgets/common';
import {SqlValue} from '../../trace_processor/query_result';
import {SegmentedButtons} from '../../widgets/segmented_buttons';
import {Trace} from '../../public/trace';
import {WATTSON_THREAD_TRACK_KIND} from './track_kinds';
import {formatPercentValue} from '../../components/aggregation_panel';

export class WattsonThreadSelectionAggregator implements Aggregator {
  readonly id = 'wattson_plugin_thread_aggregation';
  private scaleNumericData: boolean = false;

  constructor(private trace: Trace) {}

  probe(area: AreaSelection): Aggregation | undefined {
    const selectedCpus: number[] = [];
    const selectedUtids: number[] = [];
    for (const trackInfo of area.tracks) {
      if (trackInfo?.tags?.kinds?.includes(CPU_SLICE_TRACK_KIND)) {
        exists(trackInfo.tags.cpu) && selectedCpus.push(trackInfo.tags.cpu);
      }
      if (trackInfo?.tags?.kinds?.includes(WATTSON_THREAD_TRACK_KIND)) {
        exists(trackInfo.tags.utid) && selectedUtids.push(trackInfo.tags.utid);
      }
    }
    if (selectedCpus.length === 0 && selectedUtids.length === 0) {
      return undefined;
    }

    return {
      prepareData: async (engine: Engine) => {
        await engine.query(`drop view if exists ${this.id};`);
        const duration = area.end - area.start;
        const filters = [];
        if (selectedCpus.length > 0) {
          filters.push(`cpu IN (${selectedCpus.join()})`);
        }
        if (selectedUtids.length > 0) {
          filters.push(`utid IN (${selectedUtids.join()})`);
        }
        const whereClause = `WHERE ${filters.join(' OR ')}`;

        await engine.query(`
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
          ${whereClause};

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
          ${whereClause}
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
            SELECT
              utid,
              total_mws / (SUM(total_mws) OVER()) AS percent_of_total_energy
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

  renderTopbarControls(): m.Children {
    return m(SegmentedButtons, {
      options: [{label: 'µW'}, {label: 'mW'}],
      selectedOption: this.scaleNumericData ? 0 : 1,
      onOptionSelected: (index) => {
        this.scaleNumericData = index === 0;
      },
      title: 'Select power units',
    });
  }

  private renderMilliwatts(value: SqlValue): m.Children {
    if (this.scaleNumericData && typeof value === 'number') {
      return value * 1000;
    }
    return String(value);
  }

  private renderShowButton(utid: SqlValue): m.Children {
    return m(Button, {
      label: 'Show',
      intent: Intent.Primary,
      variant: ButtonVariant.Filled,
      compact: true,
      onclick: () => {
        const utidNum = typeof utid === 'number' ? utid : Number(utid);
        addWattsonThreadTrack(this.trace, utidNum);
      },
    });
  }

  getGridConfig(): AggregatorGridConfig {
    const powerUnits = this.scaleNumericData ? 'µW' : 'mW';
    const energyUnits = this.scaleNumericData ? 'µWs' : 'mWs';

    return {
      schema: {
        utid: {
          title: 'Track',
          cellRenderer: (v) => this.renderShowButton(v),
        },
        thread_name: {title: 'Thread Name', columnType: 'text'},
        tid: {title: 'TID', columnType: 'identifier'},
        pid: {title: 'PID', columnType: 'identifier'},
        active_mw: {
          title: `Active power (estimated ${powerUnits})`,
          columnType: 'quantitative',
          cellRenderer: (v) => this.renderMilliwatts(v),
        },
        active_mws: {
          title: `Active energy (estimated ${energyUnits})`,
          columnType: 'quantitative',
          cellRenderer: (v) => this.renderMilliwatts(v),
        },
        idle_cost_mws: {
          title: `Idle transitions overhead (estimated ${energyUnits})`,
          columnType: 'quantitative',
          cellRenderer: (v) => this.renderMilliwatts(v),
        },
        total_mws: {
          title: `Total energy (estimated ${energyUnits})`,
          columnType: 'quantitative',
          cellRenderer: (v) => this.renderMilliwatts(v),
        },
        percent_of_total_energy: {
          title: '% of total energy',
          columnType: 'quantitative',
          cellRenderer: formatPercentValue,
        },
      },
      initialColumns: [
        {id: 'utid', field: 'utid'},
        {id: 'thread_name', field: 'thread_name'},
        {id: 'tid', field: 'tid'},
        {id: 'pid', field: 'pid'},
        {id: 'active_mw', field: 'active_mw', aggregate: 'SUM'},
        {id: 'active_mws', field: 'active_mws', aggregate: 'SUM', sort: 'DESC'},
        {id: 'idle_cost_mws', field: 'idle_cost_mws'},
        {id: 'total_mws', field: 'total_mws', aggregate: 'SUM'},
        {id: 'percent_of_total_energy', field: 'percent_of_total_energy'},
      ],
    };
  }

  getTabName() {
    return 'Wattson by thread';
  }
}
