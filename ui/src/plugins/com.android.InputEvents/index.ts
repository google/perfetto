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

import {LONG, LONG_NULL, STR} from '../../trace_processor/query_result';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {TrackNode} from '../../public/workspace';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.InputEvents';
  static readonly dependencies = [StandardGroupsPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const cnt = await ctx.engine.query(`
      SELECT
        COUNT(*) AS cnt
      FROM slice
      WHERE name GLOB 'UnwantedInteractionBlocker::notifyMotion*'
    `);
    if (cnt.firstRow({cnt: LONG}).cnt == 0n) {
      return;
    }

    await ctx.engine.query('INCLUDE PERFETTO MODULE android.input;');
    const uri = 'com.android.InputEvents#InputEventsTrack';
    const track = await SliceTrack.createMaterialized({
      trace: ctx,
      uri,
      dataset: new SourceDataset({
        src: `
          SELECT
            read_time AS ts,
            end_to_end_latency_dur AS dur,
            CONCAT(event_type, ' ', event_action, ': ', process_name, ' (', input_event_id, ')') as name
          FROM android_input_events
          WHERE end_to_end_latency_dur IS NOT NULL
        `,
        schema: {
          ts: LONG,
          dur: LONG_NULL,
          name: STR,
        },
      }),
    });
    ctx.tracks.registerTrack({
      uri,
      renderer: track,
    });
    const node = new TrackNode({uri, name: 'Input Events'});
    const group = ctx.plugins
      .getPlugin(StandardGroupsPlugin)
      .getOrCreateStandardGroup(ctx.defaultWorkspace, 'USER_INTERACTION');
    group.addChildInOrder(node);
  }
}
