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

import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {
  SimpleCounterTrack,
  SimpleCounterTrackConfig,
} from '../../frontend/simple_counter_track';
import {TrackNode} from '../../public/workspace';
import {globals} from '../../frontend/globals';
import {getOrCreateGroupForProcess} from '../../public/standard_groups';
import {NUM} from '../../trace_processor/query_result';

class AndroidDmabuf implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    const e = ctx.engine;
    await e.query(`INCLUDE PERFETTO MODULE android.memory.dmabuf`);
    await e.query(`
      CREATE PERFETTO TABLE _android_memory_cumulative_dmabuf AS
      SELECT upid, ts, SUM(buf_size) OVER(PARTITION BY upid ORDER BY ts) AS value
      FROM android_dmabuf_allocs`);

    const pids = await e.query(
      `SELECT DISTINCT upid FROM _android_memory_cumulative_dmabuf`,
    );
    const it = pids.iter({upid: NUM});
    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const uri = `/android_process_dmabuf_${upid}`;
      const config: SimpleCounterTrackConfig = {
        data: {
          sqlSource: `SELECT ts, value FROM _android_memory_cumulative_dmabuf WHERE upid = ${upid}`,
          columns: ['ts', 'value'],
        },
        columns: {ts: 'ts', value: 'value'},
      };

      ctx.tracks.registerTrack({
        uri,
        title: `dmabuf allocs`,
        track: new SimpleCounterTrack(ctx, {trackUri: uri}, config),
      });
      const track = new TrackNode({uri, title: 'mem.dmabuf.alloc'});
      getOrCreateGroupForProcess(globals.workspace, upid).addChildInOrder(
        track,
      );
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.AndroidDmabuf',
  plugin: AndroidDmabuf,
};
