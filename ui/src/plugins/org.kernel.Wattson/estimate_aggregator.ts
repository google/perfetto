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
import {Aggregator} from '../../components/aggregation_adapter';
import {Area, AreaSelection} from '../../public/selection';
import {Engine} from '../../trace_processor/engine';
import {SqlValue} from '../../trace_processor/query_result';
import {SegmentedButtons} from '../../widgets/segmented_buttons';
import {
  CPUSS_ESTIMATE_TRACK_KIND,
  GPUSS_ESTIMATE_TRACK_KIND,
  TPUSS_ESTIMATE_TRACK_KIND,
} from './track_kinds';

export class WattsonEstimateSelectionAggregator implements Aggregator {
  readonly id = 'wattson_plugin_estimate_aggregation';
  private scaleNumericData: boolean = false;

  probe(area: AreaSelection) {
    const estimateTracks: string[] = [];
    for (const trackInfo of area.tracks) {
      if (
        (trackInfo?.tags?.kinds?.includes(CPUSS_ESTIMATE_TRACK_KIND) ||
          trackInfo?.tags?.kinds?.includes(GPUSS_ESTIMATE_TRACK_KIND) ||
          trackInfo?.tags?.kinds?.includes(TPUSS_ESTIMATE_TRACK_KIND)) &&
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

      CREATE PERFETTO VIEW ${this.id} AS
      WITH window_stats AS (
        SELECT * FROM _windowed_system_state_mw(${area.start}, ${duration})
      )
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
        ROUND(${estimateTrack}_mw, 3) as power_mw,
        ROUND(${estimateTrack}_mw * ${duration} / 1000000000, 3) as energy_mws
        FROM window_stats
      `;
    });
    query += `;`;

    return query;
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

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Name',
        columnId: 'name',
        sort: 'ASC',
      },
      {
        title: `Power (estimated ${this.powerUnits()})`,
        columnId: 'power_mw',
        sum: true,
        cellRenderer: this.renderMilliwatts.bind(this),
      },
      {
        title: `Energy (estimated ${this.powerUnits()}s)`,
        columnId: 'energy_mws',
        sum: true,
        cellRenderer: this.renderMilliwatts.bind(this),
      },
    ];
  }

  getTabName() {
    return 'Wattson estimates';
  }
}
