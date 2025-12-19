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

import {Area, AreaSelection} from '../../public/selection';
import {Engine} from '../../trace_processor/engine';
import {exists} from '../../base/utils';
import {
  CPUSS_ESTIMATE_TRACK_KIND,
  GPUSS_ESTIMATE_TRACK_KIND,
} from './track_kinds';
import {Aggregator} from '../../components/aggregation_adapter';
import {WattsonAggregationPanel} from './aggregation_panel';
import {ColumnDef} from '../../components/aggregation';

export class WattsonEstimateSelectionAggregator implements Aggregator {
  readonly id = 'wattson_plugin_estimate_aggregation';
  readonly Panel = WattsonAggregationPanel;

  probe(area: AreaSelection) {
    const estimateTracks: string[] = [];
    for (const trackInfo of area.tracks) {
      if (
        (trackInfo?.tags?.kinds?.includes(CPUSS_ESTIMATE_TRACK_KIND) ||
          trackInfo?.tags?.kinds?.includes(GPUSS_ESTIMATE_TRACK_KIND)) &&
        exists(trackInfo.tags?.wattson)
      ) {
        estimateTracks.push(`${trackInfo.tags.wattson}`);
      }
    }
    if (estimateTracks.length === 0) return undefined;

    return {
      prepareData: async (engine: Engine) => {
        await engine.query(`drop view if exists ${this.id};`);
        const query = this.getEstimateTracksQuery(area, estimateTracks);
        await engine.query(query);

        return {
          tableName: this.id,
        };
      },
    };
  }

  private getEstimateTracksQuery(
    area: Area,
    estimateTracks: ReadonlyArray<string>,
  ): string {
    const duration = area.end - area.start;
    let query = `
      INCLUDE PERFETTO MODULE wattson.estimates;

      CREATE OR REPLACE PERFETTO TABLE wattson_plugin_ui_selection_window AS
      SELECT
        ${area.start} as ts,
        ${duration} as dur;

      DROP TABLE IF EXISTS wattson_plugin_windowed_subsystems_estimate;
      CREATE VIRTUAL TABLE wattson_plugin_windowed_subsystems_estimate
      USING
        SPAN_JOIN(wattson_plugin_ui_selection_window, _system_state_mw);

      CREATE PERFETTO VIEW ${this.id} AS
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
        ROUND(SUM(${estimateTrack}_mw * dur) / ${duration}, 3) as power_mw,
        ROUND(SUM(${estimateTrack}_mw * dur) / 1000000000, 3) as energy_mws
        FROM wattson_plugin_windowed_subsystems_estimate
      `;
    });
    query += `;`;

    return query;
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Name',
        columnId: 'name',
        sort: 'ASC',
      },
      {
        title: 'Power (estimated mW)',
        columnId: 'power_mw',
        sum: true,
        formatHint: 'NUMERIC',
      },
      {
        title: 'Energy (estimated mWs)',
        columnId: 'energy_mws',
        sum: true,
        formatHint: 'NUMERIC',
      },
    ];
  }

  getTabName() {
    return 'Wattson estimates';
  }
}
