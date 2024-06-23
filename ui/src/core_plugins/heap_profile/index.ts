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

import {FlamegraphCache} from '../../core/flamegraph_cache';
import {
  LegacyFlamegraphDetailsPanel,
  profileType,
} from '../../frontend/legacy_flamegraph_panel';
import {Plugin, PluginContextTrace, PluginDescriptor} from '../../public';
import {NUM} from '../../trace_processor/query_result';
import {HeapProfileTrack} from './heap_profile_track';

export const HEAP_PROFILE_TRACK_KIND = 'HeapProfileTrack';

class HeapProfilePlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const result = await ctx.engine.query(`
      select distinct(upid) from heap_profile_allocation
      union
      select distinct(upid) from heap_graph_object
    `);
    for (const it = result.iter({upid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      ctx.registerTrack({
        uri: `perfetto.HeapProfile#${upid}`,
        displayName: 'Heap Profile',
        kind: HEAP_PROFILE_TRACK_KIND,
        upid,
        trackFactory: ({trackKey}) => {
          return new HeapProfileTrack(
            {
              engine: ctx.engine,
              trackKey,
            },
            upid,
          );
        },
      });
    }

    const cache = new FlamegraphCache('heap_profile');
    ctx.registerDetailsPanel({
      render: (sel) => {
        if (sel.kind === 'HEAP_PROFILE') {
          return m(LegacyFlamegraphDetailsPanel, {
            cache,
            selection: {
              profileType: profileType(sel.type),
              start: sel.ts,
              end: sel.ts,
              upids: [sel.upid],
            },
          });
        } else {
          return undefined;
        }
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.HeapProfile',
  plugin: HeapProfilePlugin,
};
