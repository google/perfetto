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

export class WattsonPackageAggregationController extends AggregationController {
  async createAggregateView(engine: Engine, area: Area) {
    await engine.query(`drop view if exists ${this.kind};`);

    // Short circuit if Wattson is not supported for this Perfetto trace
    if (!(await hasWattsonSupport(engine))) return false;
    const packageInfo = await engine.query(`
      INCLUDE PERFETTO MODULE android.process_metadata;
      SELECT COUNT(*) as isValid FROM android_process_metadata
      WHERE package_name IS NOT NULL
    `);
    if (packageInfo.firstRow({isValid: NUM}).isValid === 0) return false;

    const selectedCpus: number[] = [];
    for (const trackUri of area.trackUris) {
      const trackInfo = globals.trackManager.getTrack(trackUri);
      if (trackInfo?.tags?.kind === CPU_SLICE_TRACK_KIND) {
        exists(trackInfo.tags.cpu) && selectedCpus.push(trackInfo.tags.cpu);
      }
    }
    if (selectedCpus.length === 0) return false;

    const duration = area.end - area.start;

    // Prerequisite tables are already generated by Wattson thread aggregation,
    // which is run prior to execution of this module
    engine.query(`
      -- Grouped by UID and made CPU agnostic
      CREATE VIEW ${this.kind} AS
      SELECT
        ROUND(SUM(total_pws) / ${duration}, 2) as avg_mw,
        ROUND(SUM(total_pws) / 1000000000, 2) as total_mws,
        ROUND(SUM(dur) / 1000000.0, 2) as dur_ms,
        uid,
        package_name
      FROM _unioned_per_cpu_total
      GROUP BY uid;
    `);

    return true;
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Package Name',
        kind: 'STRING',
        columnConstructor: Uint16Array,
        columnId: 'package_name',
      },
      {
        title: 'Android app UID',
        kind: 'NUMBER',
        columnConstructor: Uint16Array,
        columnId: 'uid',
      },
      {
        title: 'Total Duration (ms)',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'dur_ms',
      },
      {
        title: 'Average power (estimated mW)',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'avg_mw',
        sum: true,
      },
      {
        title: 'Total energy (estimated mWs)',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'total_mws',
        sum: true,
      },
    ];
  }

  async getExtra() {}

  getTabName() {
    return 'Wattson by package';
  }

  getDefaultSorting(): Sorting {
    return {column: 'total_mws', direction: 'DESC'};
  }
}
