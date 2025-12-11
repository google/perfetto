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

import {createQueryCounterTrack} from '../../components/tracks/query_counter_track';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import {NUM, STR} from '../../trace_processor/query_result';

export default class implements PerfettoPlugin {
  static readonly id = 'com.google.PixelCpmTrace';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const group = new TrackNode({
      name: 'Central Power Manager',
      isSummary: true,
    });

    const {engine} = ctx;
    const result = await engine.query(`
      select
        id AS trackId,
        extract_arg(dimension_arg_set_id, 'name') AS trackName
      FROM track
      WHERE type = 'pixel_cpm_trace'
      ORDER BY trackName
    `);

    const it = result.iter({trackId: NUM, trackName: STR});
    for (let groupAdded = false; it.valid(); it.next()) {
      const {trackId, trackName} = it;
      const uri = `/cpm_trace_${trackName}`;
      const track = await createQueryCounterTrack({
        trace: ctx,
        uri,
        data: {
          sqlSource: `
             select ts, value
             from counter
             where track_id = ${trackId}
           `,
          columns: ['ts', 'value'],
        },
        columns: {ts: 'ts', value: 'value'},
      });
      ctx.tracks.registerTrack({
        uri,
        tags: {
          kinds: [COUNTER_TRACK_KIND],
          trackIds: [trackId],
        },
        renderer: track,
      });
      group.addChildInOrder(new TrackNode({uri, name: trackName}));
      if (!groupAdded) {
        ctx.defaultWorkspace.addChildInOrder(group);
        groupAdded = true;
      }
    }
  }
}
