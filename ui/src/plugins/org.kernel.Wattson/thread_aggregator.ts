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
import {ColumnDef} from '../../components/aggregation';
import {addWattsonThreadTrack} from './wattson_thread_utils';
import {Aggregation, Aggregator} from '../../components/aggregation_adapter';
import {AreaSelection} from '../../public/selection';
import {Button, ButtonVariant} from '../../widgets/button';
import {CPU_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {Engine} from '../../trace_processor/engine';
import {Intent} from '../../widgets/common';
import {SqlValue} from '../../trace_processor/query_result';
import {SegmentedButtons} from '../../widgets/segmented_buttons';
import {Trace} from '../../public/trace';
import {WATTSON_THREAD_TRACK_KIND} from './track_kinds';

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
          INCLUDE PERFETTO MODULE wattson.aggregation;
          CREATE OR REPLACE PERFETTO TABLE wattson_plugin_ui_selection_window AS
          SELECT
            ${area.start} as ts,
            ${duration} as dur,
            0 as period_id;

          -- Prefilter tasks table to be smaller
          CREATE OR REPLACE PERFETTO TABLE _wattson_ui_selected_tasks AS
          SELECT *
          FROM _estimates_w_tasks_attribution
          ${whereClause}
          AND ts + dur >= ${area.start}
          AND ts < ${area.end};

          -- Processes filtered by CPU within the UI defined time window
          DROP TABLE IF EXISTS wattson_plugin_windowed_summary;
          CREATE VIRTUAL TABLE wattson_plugin_windowed_summary
          USING SPAN_JOIN(
            wattson_plugin_ui_selection_window,
            _wattson_ui_selected_tasks
          );

          -- Materialize the thread-level summary once.
          CREATE OR REPLACE PERFETTO TABLE wattson_plugin_thread_summary AS
          SELECT *
          FROM _wattson_threads_aggregation!(
            wattson_plugin_windowed_summary,
            wattson_plugin_ui_selection_window
          );

          CREATE PERFETTO VIEW ${this.id} AS
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
          FROM base;
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

  private powerUnits(): string {
    return this.scaleNumericData ? 'µW' : 'mW';
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

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Track',
        columnId: 'utid',
        cellRenderer: this.renderShowButton.bind(this),
      },
      {
        title: 'Thread Name',
        columnId: 'thread_name',
      },
      {
        title: 'TID',
        columnId: 'tid',
        formatHint: 'NUMERIC',
      },
      {
        title: 'PID',
        columnId: 'pid',
        formatHint: 'NUMERIC',
      },
      {
        title: `Active power (estimated ${this.powerUnits()})`,
        columnId: 'active_mw',
        sum: true,
        cellRenderer: this.renderMilliwatts.bind(this),
      },
      {
        title: `Active energy (estimated ${this.powerUnits()}s)`,
        columnId: 'active_mws',
        sum: true,
        cellRenderer: this.renderMilliwatts.bind(this),
        sort: 'DESC',
      },
      {
        title: `Idle transitions overhead (estimated ${this.powerUnits()}s)`,
        columnId: 'idle_cost_mws',
        sum: false,
        cellRenderer: this.renderMilliwatts.bind(this),
      },
      {
        title: `Total energy (estimated ${this.powerUnits()}s)`,
        columnId: 'total_mws',
        sum: true,
        cellRenderer: this.renderMilliwatts.bind(this),
      },
      {
        title: '% of total energy',
        formatHint: 'PERCENT',
        columnId: 'percent_of_total_energy',
        sum: false,
      },
    ];
  }

  getTabName() {
    return 'Wattson by thread';
  }
}
