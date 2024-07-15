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

import {NUM, Plugin, PluginContextTrace, PluginDescriptor} from '../../public';
import {SimpleSliceTrack} from '../../frontend/simple_slice_track';

class TraceMetadata implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const res = await ctx.engine.query(`
      select count() as cnt from (select 1 from clock_snapshot limit 1)
    `);
    const row = res.firstRow({cnt: NUM});
    if (row.cnt === 0) {
      return;
    }
    ctx.registerStaticTrack({
      uri: `/clock_snapshots`,
      title: 'Clock Snapshots',
      trackFactory: (trackCtx) => {
        return new SimpleSliceTrack(ctx.engine, trackCtx, {
          data: {
            sqlSource: `
              select ts, 0 as dur, 'Snapshot' as name
              from clock_snapshot
            `,
            columns: ['ts', 'dur', 'name'],
          },
          columns: {ts: 'ts', dur: 'dur', name: 'name'},
          argColumns: [],
        });
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.TraceMetadata',
  plugin: TraceMetadata,
};
