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

import {NUM} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {SimpleSliceTrack} from '../../frontend/simple_slice_track';
import {TrackNode} from '../../public/workspace';
import {DebugSliceDetailsPanel} from '../../public/lib/debug_tracks/details_tab';
class TraceMetadata implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    const res = await ctx.engine.query(`
      select count() as cnt from (select 1 from clock_snapshot limit 1)
    `);
    const row = res.firstRow({cnt: NUM});
    if (row.cnt === 0) {
      return;
    }
    const uri = `/clock_snapshots`;
    const title = 'Clock Snapshots';
    const track = new SimpleSliceTrack(
      ctx,
      {trackUri: uri},
      {
        data: {
          sqlSource: `
            select ts, 0 as dur, 'Snapshot' as name
            from clock_snapshot
          `,
          columns: ['ts', 'dur', 'name'],
        },
        columns: {ts: 'ts', dur: 'dur', name: 'name'},
        argColumns: [],
      },
    );
    ctx.tracks.registerTrack({
      uri,
      title,
      track,
      detailsPanel: ({eventId}) =>
        new DebugSliceDetailsPanel(ctx, track.sqlTableName, eventId),
    });
    const trackNode = new TrackNode({uri, title});
    ctx.workspace.addChildInOrder(trackNode);
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.TraceMetadata',
  plugin: TraceMetadata,
};
