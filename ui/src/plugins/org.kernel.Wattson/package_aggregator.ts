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
import {
  type Aggregation,
  type Aggregator,
  type AggregatorGridConfig,
  createAggregationData,
} from '../../components/aggregation_adapter';
import type {AreaSelection} from '../../public/selection';
import {CPU_SLICE_TRACK_KIND} from '../../public/track_kinds';
import type {Engine} from '../../trace_processor/engine';
import type {SqlValue} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {RadioGroup} from '../../widgets/radio_group';
import {formatPercentValue} from '../../components/aggregation_panel';

// Base class to share logic between CPU and GPU package aggregators
abstract class WattsonBasePackageSelectionAggregator implements Aggregator {
  abstract readonly id: string;
  private scaleNumericData: boolean = false;

  probe(area: AreaSelection): Aggregation | undefined {
    const probeResult = this.doProbe(area);
    if (probeResult === undefined) return undefined;

    return {
      prepareData: async (engine: Engine) => {
        const duration = area.end - area.start;
        const preamble = this.getPreamble(area, duration, probeResult);
        if (preamble !== undefined) {
          await engine.query(preamble);
        }
        const table = await createPerfettoTable({
          engine,
          as: this.getDataQuery(area, duration, probeResult),
        });
        return createAggregationData(table);
      },
    };
  }

  // Derived classes implement this to check if they should trigger
  protected abstract doProbe(area: AreaSelection): unknown;

  protected getPreamble(
    _area: AreaSelection,
    _duration: bigint,
    _probeResult: unknown,
  ): string | undefined {
    return undefined;
  }

  // Derived classes implement this to provide the final SQL query.
  protected abstract getDataQuery(
    area: AreaSelection,
    duration: bigint,
    probeResult: unknown,
  ): string;

  abstract getTabName(): string;

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

  protected powerUnits(): string {
    return this.scaleNumericData ? 'µW' : 'mW';
  }

  protected renderMilliwatts(value: SqlValue): m.Children {
    if (typeof value === 'number') {
      const scaledValue = this.scaleNumericData ? value * 1000 : value;
      return scaledValue.toFixed(3);
    }
    return String(value);
  }

  getGridConfig(): AggregatorGridConfig {
    const powerUnits = this.powerUnits();
    const energyUnits = `${powerUnits}s`;
    const idleCost = this.hasIdleCost();

    return {
      schema: {
        package_name: {title: 'Package Name', columnType: 'text'},
        uid: {title: 'Android app UID', columnType: 'identifier'},
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
        ...(idleCost && {
          idle_cost_mws: {
            title: `Idle transitions overhead (estimated ${energyUnits})`,
            columnType: 'quantitative',
            cellRenderer: (v: SqlValue) => this.renderMilliwatts(v),
          },
        }),
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
        {id: 'package_name', field: 'package_name'},
        {id: 'uid', field: 'uid'},
        {id: 'active_mw', field: 'active_mw', aggregate: 'SUM'},
        {id: 'active_mws', field: 'active_mws', aggregate: 'SUM', sort: 'DESC'},
        ...(idleCost ? [{id: 'idle_cost_mws', field: 'idle_cost_mws'}] : []),
        {id: 'total_mws', field: 'total_mws', aggregate: 'SUM'},
        {id: 'percent_of_total_energy', field: 'percent_of_total_energy'},
      ],
    };
  }

  // Default to true, GPU override to false
  protected hasIdleCost(): boolean {
    return true;
  }
}

// Concrete implementation for CPU
export class WattsonCpuPackageSelectionAggregator extends WattsonBasePackageSelectionAggregator {
  readonly id = 'wattson_plugin_package_aggregation';

  protected doProbe(area: AreaSelection) {
    const selectedCpus: number[] = [];
    for (const trackInfo of area.tracks) {
      if (trackInfo?.tags?.kinds?.includes(CPU_SLICE_TRACK_KIND)) {
        exists(trackInfo.tags.cpu) && selectedCpus.push(trackInfo.tags.cpu);
      }
    }
    return selectedCpus.length > 0 ? {selectedCpus} : undefined;
  }

  protected getDataQuery(
    _area: AreaSelection,
    _duration: bigint,
    _probeResult: unknown,
  ): string {
    // Prerequisite tables might need to be generated if thread_aggregator didn't run,
    // but assuming it runs for now as per original code.
    return `
      -- Grouped by UID and made CPU agnostic
      WITH base AS (
        SELECT
          ROUND(SUM(estimated_mw), 3) AS active_mw,
          ROUND(SUM(estimated_mws), 3) AS active_mws,
          ROUND(SUM(idle_transitions_mws), 3) AS idle_cost_mws,
          ROUND(SUM(total_mws), 3) AS total_mws,
          package_name,
          uid
        FROM wattson_plugin_thread_summary
        GROUP BY uid, package_name
      )
      SELECT *,
        total_mws / (SUM(total_mws) OVER()) AS percent_of_total_energy
      FROM base;
    `;
  }

  getTabName() {
    return 'Wattson by package';
  }
}

// Concrete implementation for GPU
export class WattsonGpuPackageSelectionAggregator extends WattsonBasePackageSelectionAggregator {
  readonly id = 'wattson_plugin_gpu_package_aggregation';

  protected doProbe(area: AreaSelection) {
    const hasGpuWorkPeriodTrack = area.tracks.some(
      (trackInfo) => trackInfo?.pluginId === 'com.android.GpuWorkPeriod',
    );
    return hasGpuWorkPeriodTrack ? true : undefined;
  }

  protected getPreamble(
    area: AreaSelection,
    duration: bigint,
    _probeResult: unknown,
  ): string {
    return `
      INCLUDE PERFETTO MODULE wattson.estimates;
      INCLUDE PERFETTO MODULE wattson.tasks.attribution;

      CREATE OR REPLACE PERFETTO TABLE wattson_plugin_gpu_ui_selection_window AS
      SELECT
        ${area.start} as ts,
        ${duration} as dur;

      -- Filter attribution data within the UI defined time window
      DROP TABLE IF EXISTS wattson_plugin_gpu_windowed_summary;
      CREATE VIRTUAL TABLE wattson_plugin_gpu_windowed_summary
      USING SPAN_JOIN(
        wattson_plugin_gpu_ui_selection_window,
        _gpu_estimates_w_tasks_attribution
      );
    `;
  }

  protected getDataQuery(
    _area: AreaSelection,
    duration: bigint,
    _probeResult: unknown,
  ): string {
    return `
      -- Grouped by UID specifically for GPU data
      WITH base AS (
        SELECT
          ROUND(SUM(estimated_mw * dur) / ${duration}, 3) as active_mw,
          ROUND(SUM(estimated_mw * dur) / 1000000000, 3) as active_mws,
          ROUND(SUM(idle_mw * dur) / 1000000000, 3) as idle_cost_mws,
          ROUND(SUM((estimated_mw + idle_mw) * dur) / 1000000000, 3) as total_mws,
          uid,
          package_name
        FROM wattson_plugin_gpu_windowed_summary
        GROUP BY uid, package_name
      )
      select *,
        total_mws / (SUM(total_mws) OVER()) AS percent_of_total_energy
        from base;
    `;
  }

  getTabName() {
    return 'Wattson by package (GPU)';
  }
}
