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
import {addWattsonThreadTrack} from './wattson_thread_utils';
import {
  type Aggregation,
  type Aggregator,
  type AggregatorGridConfig,
  createAggregationData,
} from '../../components/aggregation_adapter';
import type {AreaSelection} from '../../public/selection';
import {Button, ButtonVariant} from '../../widgets/button';
import type {Engine} from '../../trace_processor/engine';
import {Intent} from '../../widgets/common';
import type {SqlValue} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {RadioGroup} from '../../widgets/radio_group';
import {
  type WattsonTaskSummary,
  getWattsonTrackSelection,
} from './task_summary';
import type {Trace} from '../../public/trace';
import {formatPercentValue} from '../../components/aggregation_panel';

export class WattsonThreadSelectionAggregator implements Aggregator {
  readonly id = 'wattson_plugin_thread_aggregation';
  private scaleNumericData: boolean = false;

  constructor(
    private trace: Trace,
    private readonly taskSummary: WattsonTaskSummary,
  ) {}

  probe(area: AreaSelection): Aggregation | undefined {
    const selection = getWattsonTrackSelection(area);
    if (selection.cpus.length === 0 && selection.utids.length === 0) {
      return undefined;
    }

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        await this.taskSummary.build(area, selection);

        const table = await createPerfettoTable({
          engine,
          as: `
            WITH base AS (
              SELECT
                ROUND(estimated_mw, 3) as active_mw,
                ROUND(estimated_mws, 3) as active_mws,
                ROUND(idle_transitions_mws, 3) as idle_cost_mws,
                ROUND(total_mws, 3) as total_mws,
                thread_name,
                utid,
                tid,
                pid
              FROM wattson_plugin_thread_summary
            )
            SELECT
              *,
              total_mws / (SUM(total_mws) OVER()) AS percent_of_total_energy
            FROM base
          `,
        });

        return createAggregationData(table);
      },
    };
  }

  renderTopbarControls(): m.Children {
    return m(
      RadioGroup,
      {
        selectedValue: this.scaleNumericData ? 'uw' : 'mw',
        onValueChange: (value) => {
          this.scaleNumericData = value === 'uw';
        },
        title: 'Select power units',
      },
      [
        m(RadioGroup.Button, {value: 'uw'}, 'µW'),
        m(RadioGroup.Button, {value: 'mw'}, 'mW'),
      ],
    );
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

  private getGridConfig(): AggregatorGridConfig {
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
