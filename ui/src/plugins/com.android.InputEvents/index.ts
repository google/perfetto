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

import {LONG} from '../../trace_processor/query_result';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {createQuerySliceTrack} from '../../public/lib/tracks/query_slice_track';
import {TrackNode} from '../../public/workspace';
import {getOrCreateUserInteractionGroup} from '../../public/standard_groups';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.InputEvents';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const cnt = await ctx.engine.query(`
      SELECT
        count(*) as cnt
      FROM slice
      WHERE name GLOB 'UnwantedInteractionBlocker::notifyMotion*'
    `);
    if (cnt.firstRow({cnt: LONG}).cnt == 0n) {
      return;
    }

    const SQL_SOURCE = `
      SELECT
        read_time as ts,
        end_to_end_latency_dur as dur,
        CONCAT(event_type, ' ', event_action, ': ', process_name, ' (', input_event_id, ')') as name
      FROM android_input_events
      WHERE end_to_end_latency_dur IS NOT NULL
      `;

    await ctx.engine.query('INCLUDE PERFETTO MODULE android.input;');
    const uri = 'com.android.InputEvents#InputEventsTrack';
    const title = 'Input Events';
    const track = await createQuerySliceTrack({
      trace: ctx,
      uri,
      data: {
        sqlSource: SQL_SOURCE,
      },
    });
    ctx.tracks.registerTrack({
      uri,
      title: title,
      track,
    });
    const node = new TrackNode({uri, title});
    const group = getOrCreateUserInteractionGroup(ctx.workspace);
    group.addChildInOrder(node);
  }
}
