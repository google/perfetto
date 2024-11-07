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
import {PerfettoPlugin} from '../../public/plugin';
import {CounterOptions} from '../../frontend/base_counter_track';
import {TrackNode} from '../../public/workspace';
import {createQueryCounterTrack} from '../../public/lib/tracks/query_counter_track';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.CpuidleTimeInState';
  private async addCounterTrack(
    ctx: Trace,
    name: string,
    query: string,
    group?: TrackNode,
    options?: Partial<CounterOptions>,
  ) {
    const uri = `/cpuidle_time_in_state_${name}`;
    const track = await createQueryCounterTrack({
      trace: ctx,
      uri,
      data: {
        sqlSource: query,
        columns: ['ts', 'value'],
      },
      columns: {ts: 'ts', value: 'value'},
      options,
    });
    ctx.tracks.registerTrack({
      uri,
      title: name,
      track,
    });
    const trackNode = new TrackNode({uri, title: name});
    if (group) {
      group.addChildInOrder(trackNode);
    }
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    const group = new TrackNode({
      title: 'Cpuidle Time In State',
      isSummary: true,
    });

    const e = ctx.engine;
    await e.query(`INCLUDE PERFETTO MODULE linux.cpu.idle_time_in_state;`);
    const result = await e.query(
      `select distinct state_name from cpu_idle_time_in_state_counters`,
    );
    const it = result.iter({state_name: 'str'});
    for (; it.valid(); it.next()) {
      this.addCounterTrack(
        ctx,
        it.state_name,
        `select
            ts,
            idle_percentage as value
        from cpu_idle_time_in_state_counters
        where state_name='${it.state_name}'`,
        group,
        {unit: 'percent'},
      );
    }
    if (group.hasChildren) {
      ctx.workspace.addChildInOrder(group);
    }
  }
}
