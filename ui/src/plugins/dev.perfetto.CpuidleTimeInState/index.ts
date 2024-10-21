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
import {CounterOptions} from '../../frontend/base_counter_track';
import {TrackNode} from '../../public/workspace';
import {
  SimpleCounterTrack,
  SimpleCounterTrackConfig,
} from '../../frontend/simple_counter_track';

class CpuidleTimeInState implements PerfettoPlugin {
  private addCounterTrack(
    ctx: Trace,
    name: string,
    query: string,
    group?: TrackNode,
    options?: Partial<CounterOptions>,
  ): void {
    const config: SimpleCounterTrackConfig = {
      data: {
        sqlSource: query,
        columns: ['ts', 'value'],
      },
      columns: {ts: 'ts', value: 'value'},
      options,
    };

    const uri = `/cpuidle_time_in_state_${name}`;
    ctx.tracks.registerTrack({
      uri,
      title: name,
      track: new SimpleCounterTrack(ctx, {trackUri: uri}, config),
    });
    const track = new TrackNode({uri, title: name});

    if (group) {
      group.addChildInOrder(track);
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

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.CpuidleTimeInState',
  plugin: CpuidleTimeInState,
};
