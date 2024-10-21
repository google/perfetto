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

import {ColumnDef, Sorting} from '../../public/aggregation';
import {Area, AreaSelection} from '../../public/selection';
import {Engine} from '../../trace_processor/engine';
import {CPUSS_ESTIMATE_TRACK_KIND} from '../../public/track_kinds';
import {AreaSelectionAggregator} from '../../public/selection';
import {exists} from '../../base/utils';

export class WattsonEstimateSelectionAggregator
  implements AreaSelectionAggregator
{
  readonly id = 'wattson_estimate_aggregation';

  async createAggregateView(engine: Engine, area: AreaSelection) {
    await engine.query(`drop view if exists ${this.id};`);

    const estimateTracks: string[] = [];
    for (const trackInfo of area.tracks) {
      if (
        trackInfo?.tags?.kind === CPUSS_ESTIMATE_TRACK_KIND &&
        exists(trackInfo.tags?.wattson)
      ) {
        estimateTracks.push(`${trackInfo.tags.wattson}`);
      }
    }
    if (estimateTracks.length === 0) return false;

    const query = this.getEstimateTracksQuery(area, estimateTracks);
    engine.query(query);

    return true;
  }

  getEstimateTracksQuery(
    area: Area,
    estimateTracks: ReadonlyArray<string>,
  ): string {
    const duration = area.end - area.start;
    let query = `
      INCLUDE PERFETTO MODULE wattson.curves.estimates;

      CREATE OR REPLACE PERFETTO TABLE _ui_selection_window AS
      SELECT
        ${area.start} as ts,
        ${duration} as dur;

      DROP TABLE IF EXISTS _windowed_cpuss_estimate;
      CREATE VIRTUAL TABLE _windowed_cpuss_estimate
      USING
        SPAN_JOIN(_ui_selection_window, _system_state_mw);

      CREATE VIEW ${this.id} AS
    `;

    // Convert average power track to total energy in UI window, then divide by
    // duration of window to get average estimated power of the window
    estimateTracks.forEach((estimateTrack, i) => {
      if (i != 0) {
        query += `UNION ALL `;
      }
      query += `
        SELECT
        '${estimateTrack}' as name,
        ROUND(SUM(${estimateTrack}_mw * dur) / ${duration}, 2) as power,
        ROUND(SUM(${estimateTrack}_mw * dur) / 1000000000, 2) as energy
        FROM _windowed_cpuss_estimate
      `;
    });
    query += `;`;

    return query;
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Name',
        kind: 'STRING',
        columnConstructor: Uint16Array,
        columnId: 'name',
      },
      {
        title: 'Power (estimated mW)',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'power',
        sum: true,
      },
      {
        title: 'Energy (estimated mWs)',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'energy',
        sum: true,
      },
    ];
  }

  async getExtra() {}

  getTabName() {
    return 'Wattson estimates';
  }

  getDefaultSorting(): Sorting {
    return {column: 'name', direction: 'ASC'};
  }
}
