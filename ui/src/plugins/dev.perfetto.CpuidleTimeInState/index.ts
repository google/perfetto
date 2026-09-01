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

import type {Trace} from '../../public/trace';
import type {PerfettoPlugin} from '../../public/plugin';
import {
  CounterTrack,
  type CounterTrackAttrs,
} from '../../components/tracks/counter_track';
import {TrackNode} from '../../public/workspace';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
import {getMachineCount, getTrackName} from '../../public/utils';
import {sqlValueToSqliteString} from '../../trace_processor/sql_utils';

interface MachineInfo {
  readonly id: number;
  readonly name: string | null;
  readonly labelIndex: number | null;
  readonly count: number;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.CpuidleTimeInState';
  static readonly dependencies = [StandardGroupsPlugin];

  private async addCounterTrack(
    ctx: Trace,
    uri: string,
    name: string,
    group: TrackNode,
    config: Omit<CounterTrackAttrs, 'trace' | 'uri'>,
  ) {
    const track = await CounterTrack.createMaterialized({
      trace: ctx,
      uri,
      ...config,
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
    machine: MachineInfo,
    group: TrackNode,
  ): Promise<void> {
    const name = getTrackName({
      name: `cpuidle.${state}`,
      machineName: machine.name,
      machineLabelIndex: machine.labelIndex,
      numMachines: machine.count,
    });
    await this.addCounterTrack(
      ctx,
      `/cpuidle_time_in_state_${machine.id}_${encodeURIComponent(state)}`,
      name,
      group,
      {
        unit: 'percent',
        yOverrideMaximum: 100,
        yOverrideMinimum: 0,
        sqlSource: `
        select
          ts,
          idle_percentage as value
        from linux_cpu_idle_time_in_state_counters
        where machine_id = ${machine.id}
          and state = ${sqlValueToSqliteString(state)}
      `,
      },
    );
  }

  async addPerCpuIdleStateTrack(
    ctx: Trace,
    state: string,
    cpu: number,
    machine: MachineInfo,
    group: TrackNode,
  ): Promise<void> {
    const name = getTrackName({
      name: `cpuidle.cpu${cpu}.${state} Residency`,
      machineName: machine.name,
      machineLabelIndex: machine.labelIndex,
      numMachines: machine.count,
    });
    await this.addCounterTrack(
      ctx,
      `/cpuidle_time_in_state_${machine.id}_${cpu}_${encodeURIComponent(state)}`,
      name,
      group,
      {
        unit: 'percent',
        yOverrideMaximum: 100,
        yOverrideMinimum: 0,
        sqlSource: `
        select
          ts,
          idle_percentage as value
        from linux_per_cpu_idle_time_in_state_counters
        where machine_id = ${machine.id}
          and state = ${sqlValueToSqliteString(state)}
          and cpu = ${cpu}
      `,
      },
    );
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    const group = new TrackNode({
      name: 'CPU Idle Time In State',
      isSummary: true,
    });

    const e = ctx.engine;
    const numMachines = await getMachineCount(e);
    await e.query(`INCLUDE PERFETTO MODULE linux.cpu.idle_time_in_state;`);
    const states = await e.query(
      `select distinct
         c.machine_id as machineId,
         machine.name as machineName,
         machine.label_index as machineLabelIndex,
         c.state
       from linux_cpu_idle_time_in_state_counters c
       left join machine on machine.id = c.machine_id
       order by c.machine_id, c.state`,
    );
    const it = states.iter({
      machineId: NUM,
      machineName: STR_NULL,
      machineLabelIndex: NUM_NULL,
      state: STR,
    });
    for (; it.valid(); it.next()) {
      await this.addIdleStateTrack(
        ctx,
        it.state,
        {
          id: it.machineId,
          name: it.machineName,
          labelIndex: it.machineLabelIndex,
          count: numMachines,
        },
        group,
      );
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
      `select distinct
         c.machine_id as machineId,
         machine.name as machineName,
         machine.label_index as machineLabelIndex,
         c.state,
         c.cpu
       from linux_per_cpu_idle_time_in_state_counters c
       left join machine on machine.id = c.machine_id
       order by c.machine_id, c.cpu, c.state`,
    );
    const pIt = perCpuStates.iter({
      machineId: NUM,
      machineName: STR_NULL,
      machineLabelIndex: NUM_NULL,
      state: STR,
      cpu: NUM,
    });

    for (; pIt.valid(); pIt.next()) {
      await this.addPerCpuIdleStateTrack(
        ctx,
        pIt.state,
        pIt.cpu,
        {
          id: pIt.machineId,
          name: pIt.machineName,
          labelIndex: pIt.machineLabelIndex,
          count: numMachines,
        },
        perCpuGroup,
      );
    }

    if (perCpuGroup.hasChildren) {
      const cpuGroup = ctx.plugins
        .getPlugin(StandardGroupsPlugin)
        .getOrCreateStandardGroup(ctx.defaultWorkspace, 'CPU');
      cpuGroup.addChildInOrder(perCpuGroup);
    }
  }
}
