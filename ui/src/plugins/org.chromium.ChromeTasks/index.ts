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

import {asUtid} from '../../components/sql_utils/core_types';
import {LONG_NULL, NUM, STR_NULL} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {createChromeTasksThreadTrack} from './track';
import {TrackNode} from '../../public/workspace';

export default class implements PerfettoPlugin {
  static readonly id = 'org.chromium.ChromeTasks';

  async onTraceLoad(ctx: Trace) {
    await this.createTracks(ctx);
  }

  async createTracks(ctx: Trace) {
    const it = (
      await ctx.engine.query(`
      INCLUDE PERFETTO MODULE chrome.tasks;

      with relevant_threads as (
        select distinct utid from chrome_tasks
      )
      select
        (CASE process.name
          WHEN 'Browser' THEN 1
          WHEN 'Gpu' THEN 2
          WHEN 'Renderer' THEN 4
          ELSE 3
        END) as processRank,
        process.name as processName,
        process.pid,
        process.upid,
        (CASE thread.name
          WHEN 'CrBrowserMain' THEN 1
          WHEN 'CrRendererMain' THEN 1
          WHEN 'CrGpuMain' THEN 1
          WHEN 'Chrome_IOThread' THEN 2
          WHEN 'Chrome_ChildIOThread' THEN 2
          WHEN 'VizCompositorThread' THEN 3
          WHEN 'NetworkService' THEN 3
          WHEN 'Compositor' THEN 3
          WHEN 'CompositorGpuThread' THEN 4
          WHEN 'CompositorTileWorker&' THEN 5
          WHEN 'ThreadPoolService' THEN 6
          WHEN 'ThreadPoolSingleThreadForegroundBlocking&' THEN 6
          WHEN 'ThreadPoolForegroundWorker' THEN 6
          ELSE 7
         END) as threadRank,
         thread.name as threadName,
         thread.tid,
         thread.utid
      from relevant_threads
      join thread using (utid)
      join process using (upid)
      order by processRank, upid, threadRank, utid
    `)
    ).iter({
      processRank: NUM,
      processName: STR_NULL,
      pid: LONG_NULL,
      upid: NUM,
      threadRank: NUM,
      threadName: STR_NULL,
      tid: LONG_NULL,
      utid: NUM,
    });

    const group = new TrackNode({name: 'Chrome Tasks', isSummary: true});
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const uri = `org.chromium.ChromeTasks#thread.${utid}`;
      const name = `${it.threadName} ${it.tid}`;
      ctx.tracks.registerTrack({
        uri,
        renderer: createChromeTasksThreadTrack(ctx, uri, asUtid(utid)),
      });
      const track = new TrackNode({uri, name});
      group.addChildInOrder(track);
      ctx.defaultWorkspace.addChildInOrder(group);
    }
  }
}
