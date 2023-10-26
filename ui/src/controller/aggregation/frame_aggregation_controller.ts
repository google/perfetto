// Copyright (C) 2021 The Android Open Source Project
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
import {Engine} from '../../common/engine';
import {pluginManager} from '../../common/plugins';
import {Area, Sorting} from '../../common/state';
import {globals} from '../../frontend/globals';
import {ACTUAL_FRAMES_SLICE_TRACK_KIND} from '../../tracks/actual_frames';

import {AggregationController} from './aggregation_controller';

export class FrameAggregationController extends AggregationController {
  async createAggregateView(engine: Engine, area: Area) {
    await engine.query(`drop view if exists ${this.kind};`);

    const selectedSqlTrackIds: number[] = [];
    for (const trackKey of area.tracks) {
      const track = globals.state.tracks[trackKey];
      // Track will be undefined for track groups.
      if (track?.uri !== undefined) {
        const trackInfo = pluginManager.resolveTrackInfo(track.uri);
        if (trackInfo?.kind === ACTUAL_FRAMES_SLICE_TRACK_KIND) {
          trackInfo.trackIds && selectedSqlTrackIds.push(...trackInfo.trackIds);
        }
      }
    }
    if (selectedSqlTrackIds.length === 0) return false;

    const query = `create view ${this.kind} as
        SELECT
        jank_type,
        count(1) as occurrences,
        MIN(dur) as minDur,
        AVG(dur) as meanDur,
        MAX(dur) as maxDur
        FROM actual_frame_timeline_slice
        WHERE track_id IN (${selectedSqlTrackIds}) AND
        ts + dur > ${area.start} AND
        ts < ${area.end} group by jank_type`;

    await engine.query(query);
    return true;
  }

  getTabName() {
    return 'Frames';
  }

  async getExtra() {}

  getDefaultSorting(): Sorting {
    return {column: 'occurrences', direction: 'DESC'};
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Jank Type',
        kind: 'STRING',
        columnConstructor: Uint16Array,
        columnId: 'jank_type',
      },
      {
        title: 'Min duration',
        kind: 'NUMBER',
        columnConstructor: Uint16Array,
        columnId: 'minDur',
      },
      {
        title: 'Max duration',
        kind: 'NUMBER',
        columnConstructor: Uint16Array,
        columnId: 'maxDur',
      },
      {
        title: 'Mean duration',
        kind: 'NUMBER',
        columnConstructor: Uint16Array,
        columnId: 'meanDur',
      },
      {
        title: 'Occurrences',
        kind: 'NUMBER',
        columnConstructor: Uint16Array,
        columnId: 'occurrences',
        sum: true,
      },
    ];
  }
}
