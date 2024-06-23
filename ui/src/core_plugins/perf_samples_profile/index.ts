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

import {TrackData} from '../../common/track_data';
import {PERF_SAMPLES_PROFILE_TRACK_KIND} from '../../public';
import {FlamegraphCache} from '../../core/flamegraph_cache';
import {
  LegacyFlamegraphDetailsPanel,
  profileType,
} from '../../frontend/legacy_flamegraph_panel';
import {Plugin, PluginContextTrace, PluginDescriptor} from '../../public';
import {NUM} from '../../trace_processor/query_result';
import {PerfSamplesProfileTrack} from './perf_samples_profile_track';

export interface Data extends TrackData {
  tsStarts: BigInt64Array;
}

class PerfSamplesProfilePlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const result = await ctx.engine.query(`
      select distinct upid, pid
      from perf_sample join thread using (utid) join process using (upid)
      where callsite_id is not null
    `);
    for (const it = result.iter({upid: NUM, pid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      const pid = it.pid;
      ctx.registerTrack({
        uri: `perfetto.PerfSamplesProfile#${upid}`,
        displayName: `Callstacks ${pid}`,
        kind: PERF_SAMPLES_PROFILE_TRACK_KIND,
        upid,
        trackFactory: () => new PerfSamplesProfileTrack(ctx.engine, upid),
      });
    }

    const cache = new FlamegraphCache('perf_samples');
    ctx.registerDetailsPanel({
      render: (sel) => {
        if (sel.kind === 'PERF_SAMPLES') {
          return m(LegacyFlamegraphDetailsPanel, {
            cache,
            selection: {
              profileType: profileType(sel.type),
              start: sel.leftTs,
              end: sel.rightTs,
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
  pluginId: 'perfetto.PerfSamplesProfile',
  plugin: PerfSamplesProfilePlugin,
};
