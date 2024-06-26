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

import {ColumnDef} from '../../common/aggregation_data';
import {Area, Sorting} from '../../common/state';
import {globals} from '../../frontend/globals';
import {Engine} from '../../trace_processor/engine';
import {CPUSS_ESTIMATE_TRACK_KIND} from '../../core/track_kinds';
import {AggregationController} from './aggregation_controller';

export class WattsonAggregationController extends AggregationController {
  async createAggregateView(engine: Engine, area: Area) {
    await engine.query(`drop view if exists ${this.kind};`);

    const estimateTracks: (string | undefined)[] = [];
    for (const trackKey of area.tracks) {
      const track = globals.state.tracks[trackKey];
      if (track?.uri) {
        const trackInfo = globals.trackManager.resolveTrackInfo(track.uri);
        if (trackInfo?.kind === CPUSS_ESTIMATE_TRACK_KIND) {
          const estimateTrack = track.uri.toLowerCase().split(`#`).pop();
          estimateTracks.push(estimateTrack);
        }
      }
    }
    if (estimateTracks.length === 0) return false;

    const query = this.getEstimateTracksQuery(area, estimateTracks);
    engine.query(query);

    return true;
  }

  getEstimateTracksQuery(
    area: Area,
    estimateTracks: (string | undefined)[],
  ): string {
    const duration = area.end - area.start;
    let query = `
      DROP TABLE IF EXISTS _ss_converted_to_mw;
      CREATE PERFETTO TABLE _ss_converted_to_mw AS
      SELECT *,
        ((IFNULL(l3_hit_value, 0) + IFNULL(l3_miss_value, 0)) * 1000 / dur)
          + static_curve as scuinterconnect_curve
      FROM _system_state_curves;

      DROP TABLE IF EXISTS _ui_selection_window;
      CREATE PERFETTO TABLE _ui_selection_window AS
      SELECT
        ${area.start} as ts,
        ${duration} as dur;

      DROP TABLE IF EXISTS _windowed_cpuss_estimate;
      CREATE VIRTUAL TABLE _windowed_cpuss_estimate
      USING
        SPAN_JOIN(_ui_selection_window, _ss_converted_to_mw);

      CREATE VIEW ${this.kind} AS
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
        ROUND(SUM(${estimateTrack}_curve * dur) / ${duration}, 2) as value
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
        title: 'Average estimate (mW)',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'value',
      },
    ];
  }

  async getExtra() {}

  getTabName() {
    return 'Power Estimates';
  }

  getDefaultSorting(): Sorting {
    return {column: 'name', direction: 'ASC'};
  }
}
