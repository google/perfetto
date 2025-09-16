// Copyright (C) 2025 The Android Open Source Project
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
import {PerfettoPlugin} from '../../public/plugin';
import {TrackNode} from '../../public/workspace';
import {createQueryCounterTrack} from '../../components/tracks/query_counter_track';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import {NUM, STR} from '../../trace_processor/query_result';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.CpuPerUid';
  static readonly dependencies = [StandardGroupsPlugin];

  private async addCpuPerUidTrack(
    ctx: Trace,
    trackId: number,
    name: string,
    group: TrackNode,
  ) {
    const uri = `/cpu_per_uid_${trackId}`;
    const track = await createQueryCounterTrack({
      trace: ctx,
      uri,
      data: {
        sqlSource: `select
           ts,
           min(100, 100 * cpu_ratio) as value
         from android_cpu_per_uid_counter
         where track_id = ${trackId}`,
        columns: ['ts', 'value'],
      },
      columns: {ts: 'ts', value: 'value'},
      options: {
        unit: 'percent',
        yOverrideMaximum: 100,
        yOverrideMinimum: 0,
        yRangeSharingKey: 'cpu-per-uid',
      },
    });
    ctx.tracks.registerTrack({
      uri,
      renderer: track,
    });
    const node = new TrackNode({uri, name});
    group.addChildInOrder(node);
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    const e = ctx.engine;
    await e.query(`INCLUDE PERFETTO MODULE android.cpu.cpu_per_uid;`);
    const tracks = await e.query(
      `select
          id, 
          cluster, 
          ifnull(package_name, 'UID ' || uid) as name
        from android_cpu_per_uid_track
        order by name, cluster`,
    );
    const it = tracks.iter({id: NUM, cluster: NUM, name: STR});
    if (it.valid()) {
      const group = new TrackNode({
        name: 'CPU Per UID',
        isSummary: true,
      });
      ctx.workspace.addChildInOrder(group);

      for (; it.valid(); it.next()) {
        const name = `${it.name} (${it.cluster})`;
        await this.addCpuPerUidTrack(ctx, it.id, name, group);
      }
    }
  }
}
