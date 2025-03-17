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

import {exists} from '../../base/utils';
import {ColumnDef, Sorting} from '../../public/aggregation';
import {AreaSelection} from '../../public/selection';
import {Engine} from '../../trace_processor/engine';
import {NUM} from '../../trace_processor/query_result';
import {CPU_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {AreaSelectionAggregator} from '../../public/selection';

export class WattsonPackageSelectionAggregator
  implements AreaSelectionAggregator
{
  readonly id = 'wattson_package_aggregation';

  async createAggregateView(engine: Engine, area: AreaSelection) {
    await engine.query(`drop view if exists ${this.id};`);

    const packageInfo = await engine.query(`
      INCLUDE PERFETTO MODULE android.process_metadata;
      SELECT COUNT(*) as isValid FROM android_process_metadata
      WHERE package_name IS NOT NULL
    `);
    if (packageInfo.firstRow({isValid: NUM}).isValid === 0) return false;

    const selectedCpus: number[] = [];
    for (const trackInfo of area.tracks) {
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
      CREATE PERFETTO VIEW ${this.id} AS
      SELECT
        ROUND(SUM(total_pws) / ${duration}, 3) as active_mw,
        ROUND(SUM(total_pws) / 1000000000, 3) as active_mws,
        ROUND(SUM(dur) / 1000000.0, 3) as dur_ms,
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
    ];
  }

  async getExtra() {}

  getTabName() {
    return 'Wattson by package';
  }

  getDefaultSorting(): Sorting {
    return {column: 'active_mws', direction: 'DESC'};
  }
}
