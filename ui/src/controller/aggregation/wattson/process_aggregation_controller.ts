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

import {exists} from '../../../base/utils';
import {ColumnDef} from '../../../common/aggregation_data';
import {Area, Sorting} from '../../../common/state';
import {globals} from '../../../frontend/globals';
import {Engine} from '../../../trace_processor/engine';
import {NUM} from '../../../trace_processor/query_result';
import {CPU_SLICE_TRACK_KIND} from '../../../core/track_kinds';
import {AggregationController} from '../aggregation_controller';
import {hasWattsonSupport} from '../../../core/trace_config_utils';

export class WattsonProcessAggregationController extends AggregationController {
  async createAggregateView(engine: Engine, area: Area) {
    await engine.query(`drop view if exists ${this.kind};`);

    // Short circuit if Wattson is not supported for this Perfetto trace
    if (!(await hasWattsonSupport(engine))) return false;
    const deviceInfo = await engine.query(`
        INCLUDE PERFETTO MODULE wattson.device_infos;
        SELECT COUNT(*) as isValid FROM _wattson_device
    `);
    if (deviceInfo.firstRow({isValid: NUM}).isValid === 0) return false;

    const selectedCpus: number[] = [];
    for (const trackKey of area.tracks) {
      const track = globals.state.tracks[trackKey];
      if (track?.uri) {
        const trackInfo = globals.trackManager.resolveTrackInfo(track.uri);
        if (trackInfo?.tags?.kind === CPU_SLICE_TRACK_KIND) {
          exists(trackInfo.tags.cpu) && selectedCpus.push(trackInfo.tags.cpu);
        }
      }
    }
    if (selectedCpus.length === 0) return false;

    const duration = area.end - area.start;
    engine.query(`
      INCLUDE PERFETTO MODULE viz.summary.threads_w_processes;

      CREATE OR REPLACE PERFETTO TABLE _ui_selection_window AS
      SELECT
        ${area.start} as ts,
        ${duration} as dur;

      -- Processes filtered by CPU within the UI defined time window
      DROP TABLE IF EXISTS _windowed_summary;
      CREATE VIRTUAL TABLE _windowed_summary
      USING
        SPAN_JOIN(_ui_selection_window, _sched_w_thread_process_package_summary);
    `);
    this.runEstimateProcessQuery(engine, selectedCpus, duration);

    return true;
  }

  // This function returns a query that gets the average and estimate from
  // Wattson for the selection in the UI window based on process. The grouping
  // by thread needs to 'remove' 2 dimensions; the threads need to be grouped
  // over time and the processes need to be grouped over CPUs.
  // 1. Window and associate process with proper Wattson estimate slice
  // 2. Group all processes over time on a per CPU basis
  // 3. Group all processes over all CPUs
  runEstimateProcessQuery(
    engine: Engine,
    selectedCpus: number[],
    duration: bigint,
  ){
    // Estimate and total per UPID per CPU
    selectedCpus.forEach((cpu) => {
      engine.query(`
        -- Packages filtered by CPU
        CREATE OR REPLACE PERFETTO VIEW _windowed_summary_per_cpu${cpu} AS
        SELECT ts, dur, cpu, utid, upid, pid, thread_name, process_name
        FROM _windowed_summary WHERE cpu = ${cpu};

        -- CPU specific track with slices for curves
        CREATE OR REPLACE PERFETTO VIEW _per_cpu_curve${cpu} AS
        SELECT ts, dur, cpu${cpu}_curve
        FROM _system_state_curves;

        -- Filter out track when threads are available
        DROP TABLE IF EXISTS _windowed_process_curve${cpu};
        CREATE VIRTUAL TABLE _windowed_process_curve${cpu}
        USING
          SPAN_JOIN(_per_cpu${cpu}_curve, _windowed_summary_per_cpu${cpu});

        -- Total estimate per UPID per CPU
        CREATE OR REPLACE PERFETTO TABLE _total_per_process_cpu${cpu} AS
        SELECT
          SUM(cpu${cpu}_curve * dur) as total_pws,
          SUM(dur) as dur,
          upid,
          pid,
          process_name
        FROM _windowed_process_curve${cpu}
        GROUP BY upid;
      `);
    });

    // Estimate and total per UPID, removing CPU dimension
    let query = `
      -- Grouped again by UPID, but this time to make it CPU agnostic
      CREATE VIEW ${this.kind} AS
      WITH _unioned_per_process_per_cpu AS (
    `;
    selectedCpus.forEach((cpu, i) => {
      query += i != 0 ? `UNION ALL\n` : ``;
      query += `SELECT * from _total_per_process_cpu${cpu}\n`;
    });
    query += `
      )
      SELECT
        ROUND(SUM(total_pws) / ${duration}, 2) as avg_mw,
        ROUND(SUM(total_pws) / 1000000000, 2) as total_mws,
        pid,
        process_name
      FROM _unioned_per_process_per_cpu
      GROUP BY upid;
    `;

    engine.query(query);

    return;
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Process Name',
        kind: 'STRING',
        columnConstructor: Uint16Array,
        columnId: 'process_name',
      },
      {
        title: 'PID',
        kind: 'NUMBER',
        columnConstructor: Uint16Array,
        columnId: 'pid',
      },
      {
        title: 'Average estimated power (mW)',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'avg_mw',
        sum: true,
      },
      {
        title: 'Total estimated energy (mWs)',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'total_mws',
        sum: true,
      },
    ];
  }

  async getExtra() {}

  getTabName() {
    return 'Wattson by process';
  }

  getDefaultSorting(): Sorting {
    return {column: 'total_mws', direction: 'DESC'};
  }
}
