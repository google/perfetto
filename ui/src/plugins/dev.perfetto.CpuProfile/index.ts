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

import {CPU_PROFILE_TRACK_KIND} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {CpuProfileTrack} from './cpu_profile_track';
import {getThreadUriPrefix} from '../../public/utils';
import {exists} from '../../base/utils';
import {getOrCreateGroupForThread} from '../../public/standard_groups';
import {TrackNode} from '../../public/workspace';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.CpuProfile';
  async onTraceLoad(ctx: Trace): Promise<void> {
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
      join thread using(utid)
    `);

    const it = result.iter({
      utid: NUM,
      upid: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const upid = it.upid;
      const threadName = it.threadName;
      const uri = `${getThreadUriPrefix(upid, utid)}_cpu_samples`;
      const title = `${threadName} (CPU Stack Samples)`;
      ctx.tracks.registerTrack({
        uri,
        title,
        tags: {
          kind: CPU_PROFILE_TRACK_KIND,
          utid,
          ...(exists(upid) && {upid}),
        },
        track: new CpuProfileTrack(
          {
            trace: ctx,
            uri,
          },
          utid,
        ),
      });
      const group = getOrCreateGroupForThread(ctx.workspace, utid);
      const track = new TrackNode({uri, title, sortOrder: -40});
      group.addChildInOrder(track);
    }
  }
}
