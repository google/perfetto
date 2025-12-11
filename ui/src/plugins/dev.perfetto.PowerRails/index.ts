// Copyright (C) 2025 The Android Open Source Project
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

import {createAggregationTab} from '../../components/aggregation_adapter';
import {createQueryCounterTrack} from '../../components/tracks/query_counter_track';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {getTrackName} from '../../public/utils';
import {TrackNode} from '../../public/workspace';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import {PowerCounterSelectionAggregator} from './power_counter_selection_aggregator';

/**
 * This plugin handles power rail counter tracks.
 */
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.PowerRails';
  static readonly dependencies = [StandardGroupsPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new PowerCounterSelectionAggregator(), 200),
    );

    await this.addPowerRailCounterTracks(ctx);
  }

  private async addPowerRailCounterTracks(ctx: Trace): Promise<void> {
    const result = await ctx.engine.query(`
      INCLUDE PERFETTO MODULE android.power_rails;

      SELECT
        track_id as trackId,
        COALESCE(friendly_name, raw_power_rail_name) as name,
        machine_id as machine
      FROM android_power_rails_metadata
      ORDER BY machine_id, name
    `);

    if (result.numRows() === 0) {
      return;
    }

    const it = result.iter({
      trackId: NUM,
      name: STR_NULL,
      machine: NUM_NULL,
    });

    const powerRailsGroup = new TrackNode({
      name: 'Power Rails',
      isSummary: true,
    });
    ctx.plugins
      .getPlugin(StandardGroupsPlugin)
      .getOrCreateStandardGroup(ctx.defaultWorkspace, 'POWER')
      .addChildInOrder(powerRailsGroup);

    for (; it.valid(); it.next()) {
      const {trackId, name, machine} = it;
      const trackName = getTrackName({
        name,
        kind: COUNTER_TRACK_KIND,
        machine,
      });
      const uri = `/counter_${trackId}`;
      const track = await createQueryCounterTrack({
        trace: ctx,
        uri,
        data: {
          sqlSource: `
            SELECT
              ts,
              value / 1000.0 AS value -- convert uJ to mJ
            FROM counter
            WHERE track_id = ${trackId}
          `,
        },
        options: {
          yMode: 'rate',
          yRangeSharingKey: 'power_rails',
          unit: 'mJ',
          rateUnit: 'mW',
        },
      });

      ctx.tracks.registerTrack({
        uri,
        tags: {
          kinds: [COUNTER_TRACK_KIND],
          trackIds: [trackId],
          type: 'power_rails',
        },
        renderer: track,
      });

      powerRailsGroup.addChildInOrder(
        new TrackNode({
          uri,
          name: trackName,
        }),
      );
    }
  }
}
