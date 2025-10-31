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
import {CounterOptions} from '../../components/tracks/base_counter_track';
import {TrackNode} from '../../public/workspace';
import {createQueryCounterTrack} from '../../components/tracks/query_counter_track';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import {NUM, STR} from '../../trace_processor/query_result';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.CpuidleTimeInState';
  static readonly dependencies = [StandardGroupsPlugin];

  private async addCounterTrack(
    ctx: Trace,
    name: string,
    query: string,
    group: TrackNode,
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
      renderer: track,
    });
    const node = new TrackNode({uri, name});
    group.addChildInOrder(node);
  }

  async addIdleStateTrack(
    ctx: Trace,
    state: string,
    group: TrackNode,
  ): Promise<void> {
    await this.addCounterTrack(
      ctx,
      `cpuidle.${state}`,
      `
        select
          ts,
          idle_percentage as value
        from linux_cpu_idle_time_in_state_counters
        where state = '${state}'
      `,
      group,
      {unit: 'percent', yOverrideMaximum: 100, yOverrideMinimum: 0},
    );
  }

  async addPerCpuIdleStateTrack(
    ctx: Trace,
    state: string,
    cpu: number,
    group: TrackNode,
  ): Promise<void> {
    await this.addCounterTrack(
      ctx,
      `cpuidle.cpu${cpu}.${state} Residency`,
      `
        select
          ts,
          idle_percentage as value
        from linux_per_cpu_idle_time_in_state_counters
        where state = '${state}' AND cpu = ${cpu}
      `,
      group,
      {unit: 'percent', yOverrideMaximum: 100, yOverrideMinimum: 0},
    );
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    const group = new TrackNode({
      name: 'CPU Idle Time In State',
      isSummary: true,
    });

    const e = ctx.engine;
    await e.query(`INCLUDE PERFETTO MODULE linux.cpu.idle_time_in_state;`);
    const states = await e.query(
      `select distinct state from linux_cpu_idle_time_in_state_counters`,
    );
    const it = states.iter({state: STR});
    for (; it.valid(); it.next()) {
      await this.addIdleStateTrack(ctx, it.state, group);
    }

    if (group.hasChildren) {
      const cpuGroup = ctx.plugins
        .getPlugin(StandardGroupsPlugin)
        .getOrCreateStandardGroup(ctx.defaultWorkspace, 'CPU');
      cpuGroup.addChildInOrder(group);
    }

    const perCpuGroup = new TrackNode({
      name: 'CPU Idle Per Cpu Time In State',
      isSummary: true,
    });

    const perCpuStates = await e.query(
      `select distinct state, cpu from linux_per_cpu_idle_time_in_state_counters`,
    );
    const pIt = perCpuStates.iter({state: STR, cpu: NUM});

    for (; pIt.valid(); pIt.next()) {
      await this.addPerCpuIdleStateTrack(ctx, pIt.state, pIt.cpu, perCpuGroup);
    }

    if (perCpuGroup.hasChildren) {
      const cpuGroup = ctx.plugins
        .getPlugin(StandardGroupsPlugin)
        .getOrCreateStandardGroup(ctx.defaultWorkspace, 'CPU');
      cpuGroup.addChildInOrder(perCpuGroup);
    }
  }
}
