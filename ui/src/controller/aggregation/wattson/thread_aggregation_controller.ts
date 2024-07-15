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
import {CPU_SLICE_TRACK_KIND} from '../../../core/track_kinds';
import {AggregationController} from '../aggregation_controller';
import {hasWattsonSupport} from '../../../core/trace_config_utils';

export class WattsonThreadAggregationController extends AggregationController {
  async createAggregateView(engine: Engine, area: Area) {
    await engine.query(`drop view if exists ${this.kind};`);

    // Short circuit if Wattson is not supported for this Perfetto trace
    if (!(await hasWattsonSupport(engine))) return false;

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
    const queryPrefix = `
      DROP TABLE IF EXISTS _ui_selection_window;
      CREATE PERFETTO TABLE _ui_selection_window AS
      SELECT
        ${area.start} as ts,
        ${duration} as dur;
    `;
    engine.query(
      this.getEstimateThreadsQuery(queryPrefix, selectedCpus, duration),
    );

    return true;
  }

  // This function returns a query that gets the average and estimate from
  // Wattson for the selection in the UI window based on thread. The grouping by
  // thread needs to 'remove' 2 dimensions; the threads need to be grouped over
  // time and the threads need to be grouped over CPUs.
  // 1. Window and associate thread with proper Wattson estimate slice
  // 2. Group all threads over time on a per CPU basis
  // 3. Group all threads over all CPUs
  getEstimateThreadsQuery(
    queryPrefix: string,
    selectedCpu: number[],
    duration: bigint,
  ): string {
    let query = queryPrefix;

    // Estimate and total per UTID per CPU
    selectedCpu.forEach((cpu) => {
      query += `
        -- Threads filtered by CPU
        DROP TABLE IF EXISTS _per_cpu_threads;
        CREATE PERFETTO TABLE _per_cpu_threads AS
        SELECT ts, dur, cpu, utid
        FROM sched WHERE cpu = ${cpu};

        -- Threads filtered by CPU within the UI defined time window
        DROP TABLE IF EXISTS _windowed_per_cpu_threads;
        CREATE VIRTUAL TABLE _windowed_per_cpu_threads
        USING
          SPAN_JOIN(_ui_selection_window, _per_cpu_threads);

        -- CPU specific track with slices for curves
        DROP TABLE IF EXISTS _per_cpu_curve;
        CREATE PERFETTO TABLE _per_cpu_curve AS
        SELECT ts, dur, cpu${cpu}_curve
        FROM _system_state_curves;

        -- Filter out track when threads are available
        DROP TABLE IF EXISTS _windowed_thread_curve;
        CREATE VIRTUAL TABLE _windowed_thread_curve
        USING SPAN_JOIN(_per_cpu_curve, _windowed_per_cpu_threads);

        -- Total estimate per UTID per CPU
        DROP TABLE IF EXISTS _total_per_thread_cpu${cpu};
        CREATE PERFETTO TABLE _total_per_thread_cpu${cpu} AS
        SELECT
          SUM(cpu${cpu}_curve * dur) as total_pws,
          SUM(dur) as dur,
          COUNT(dur) as occurences,
          utid,
          cpu
        FROM _windowed_thread_curve
        GROUP BY utid;
      `;
    });

    // Estimate and total per UTID, removing CPU dimension
    query += `
      -- Grouped again by UTID, but this time to make it CPU agnostic
      DROP TABLE IF EXISTS _total_per_thread;
      CREATE PERFETTO TABLE _total_per_thread AS
      WITH _unioned_per_thread_per_cpu AS (
    `;
    selectedCpu.forEach((cpu, i) => {
      query += i != 0 ? `UNION ALL\n` : ``;
      query += `SELECT * from _total_per_thread_cpu${cpu}\n`;
    });
    query += `
      )
      SELECT
        ROUND(SUM(total_pws) / ${duration}, 2) as avg_mw,
        ROUND(SUM(total_pws) / 1000000000, 2) as total_mws,
        utid
      FROM _unioned_per_thread_per_cpu
      GROUP BY utid;
    `;

    // Final table outputted in UI
    query += `
      CREATE VIEW ${this.kind} AS
      SELECT tpt.*, thread.name as t_name, thread.tid, process.pid
      FROM _total_per_thread as tpt
      JOIN thread on tpt.utid = thread.utid
      LEFT JOIN process on thread.upid = process.upid;
    `;

    return query;
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Thread Name',
        kind: 'STRING',
        columnConstructor: Uint16Array,
        columnId: 't_name',
      },
      {
        title: 'TID',
        kind: 'NUMBER',
        columnConstructor: Uint16Array,
        columnId: 'tid',
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
    return 'Wattson by thread';
  }

  getDefaultSorting(): Sorting {
    return {column: 'total_mws', direction: 'DESC'};
  }
}
