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

import {CpuProfileDetailsPanel} from '../../frontend/cpu_profile_panel';
import {Plugin, PluginContextTrace, PluginDescriptor} from '../../public';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {CpuProfileTrack} from './cpu_profile_track';

export const CPU_PROFILE_TRACK_KIND = 'CpuProfileTrack';

class CpuProfile implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const result = await ctx.engine.query(`
      with thread_cpu_sample as (
        select distinct utid
        from cpu_profile_stack_sample
        where utid != 0
      )
      select
        utid,
        tid,
        upid,
        thread.name as threadName
      from thread_cpu_sample
      join thread using(utid)`);

    const it = result.iter({
      utid: NUM,
      upid: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const threadName = it.threadName;
      ctx.registerTrack({
        uri: `perfetto.CpuProfile#${utid}`,
        displayName: `${threadName} (CPU Stack Samples)`,
        kind: CPU_PROFILE_TRACK_KIND,
        utid,
        trackFactory: () => new CpuProfileTrack(ctx.engine, utid),
      });
    }

    ctx.registerDetailsPanel({
      render: (sel) => {
        if (sel.kind === 'CPU_PROFILE_SAMPLE') {
          return m(CpuProfileDetailsPanel);
        } else {
          return undefined;
        }
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.CpuProfile',
  plugin: CpuProfile,
};
