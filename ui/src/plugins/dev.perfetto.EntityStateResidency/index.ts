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

import {TrackNode} from '../../public/workspace';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {createAggregationToTabAdaptor} from '../../components/aggregation_adapter';
import {PerfettoPlugin} from '../../public/plugin';
import {NUM, STR} from '../../trace_processor/query_result';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import {Trace} from '../../public/trace';
import {EntityStateResidencySelectionAggregator} from './entity_state_residency_selection_aggregator';
import {createQueryCounterTrack} from '../../components/tracks/query_counter_track';

/**
 * This plugin handles the aggregations for entity state residency counter tracks.
 */
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.EntityStateResidency';
  static readonly dependencies = [StandardGroupsPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.selection.registerAreaSelectionTab(
      createAggregationToTabAdaptor(
        ctx,
        new EntityStateResidencySelectionAggregator(),
        200,
      ),
    );

    const result = await ctx.engine.query(`
          INCLUDE PERFETTO MODULE android.entity_state_residency;
          SELECT
            entity_name AS entity,
            state_name AS state,
            track_id AS trackId
          FROM android_entity_state_residency
          GROUP BY
            entity_name, state_name, track_id
          ORDER BY
            entity_name, state_name
        `);

    let entityResidencyGroup: TrackNode | undefined;
    let currentGroup: TrackNode | undefined;
    const it = result.iter({entity: STR, state: STR, trackId: NUM});
    for (; it.valid(); it.next()) {
      if (!entityResidencyGroup) {
        const powerGroup = ctx.plugins
          .getPlugin(StandardGroupsPlugin)
          .getOrCreateStandardGroup(ctx.workspace, 'POWER');
        entityResidencyGroup = new TrackNode({title: 'Entity Residency'});
        powerGroup.addChildInOrder(entityResidencyGroup);
      }

      // Create a track group for the current entity if it does not already
      // exist.
      if (currentGroup?.title !== it.entity) {
        currentGroup = new TrackNode({title: it.entity, isSummary: true});
        entityResidencyGroup.addChildInOrder(currentGroup);
      }

      // Create and register a track for the state residency.
      const uri = `/entity_state_residency_${it.entity}_${it.state}`;
      const name = it.state;
      const track = await createQueryCounterTrack({
        trace: ctx,
        uri,
        data: {
          sqlSource: `
              SELECT
                ts,
                state_time_since_boot / 1e9 AS value
              FROM android_entity_state_residency
              WHERE track_id = ${it.trackId}
            `,
          columns: ['ts', 'value'],
        },
        columns: {ts: 'ts', value: 'value'},
        options: {
          yMode: 'rate',
          yRangeSharingKey: `entity_state_residency_${it.entity}`,
          unit: 's',
        },
      });

      ctx.tracks.registerTrack({
        uri,
        title: name,
        tags: {
          kind: COUNTER_TRACK_KIND,
          trackIds: [it.trackId],
          type: 'entity_state',
        },
        track,
      });
      currentGroup.addChildInOrder(new TrackNode({uri, title: name}));
    }
  }
}
