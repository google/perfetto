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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {CPU_FREQ_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import {NUM, NUM_NULL} from '../../trace_processor/query_result';
import {CpuFreqTrack} from './cpu_freq_track';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.CpuFreq';
  async onTraceLoad(ctx: Trace): Promise<void> {
    const {engine} = ctx;

    // ctx.traceInfo.cpus contains all cpus seen from all events. Filter the set
    // if it's seen in cpu_counter_track.
    const queryRes = await ctx.engine.query(
      `select distinct cpu, ifnull(machine_id, 0) as machine
       from cpu_counter_track`,
    );
    const cpuAndMachine = new Set<string>();
    for (
      const it = queryRes.iter({cpu: NUM, machine: NUM});
      it.valid();
      it.next()
    ) {
      cpuAndMachine.add([it.cpu, it.machine].toString());
    }
    const cpus = ctx.traceInfo.cpus.filter((cpu) =>
      cpuAndMachine.has([cpu.cpu, cpu.machine].toString()),
    );

    const maxCpuFreqResult = await engine.query(`
      select ifnull(max(value), 0) as freq
      from counter c
      join cpu_counter_track t on c.track_id = t.id
      join _counter_track_summary s on t.id = s.id
      where t.type = 'cpu_frequency';
    `);
    const maxCpuFreq = maxCpuFreqResult.firstRow({freq: NUM}).freq;

    for (const cpu of cpus) {
      // Only add a cpu freq track if we have cpu freq data.
      const cpuFreqIdleResult = await engine.query(`
        select
          id as cpuFreqId,
          (
            select id
            from cpu_counter_track t
            where t.type = 'cpu_idle'
            and t.cpu = ${cpu.cpu} and ifnull(t.machine_id, 0) = ${cpu.machine}
            limit 1
          ) as cpuIdleId
        from cpu_counter_track t
        join _counter_track_summary using (id)
        where t.type = 'cpu_frequency'
        and t.cpu = ${cpu.cpu} and ifnull(t.machine_id, 0) = ${cpu.machine}
        limit 1;
      `);

      if (cpuFreqIdleResult.numRows() > 0) {
        const row = cpuFreqIdleResult.firstRow({
          cpuFreqId: NUM,
          cpuIdleId: NUM_NULL,
        });
        const freqTrackId = row.cpuFreqId;
        const idleTrackId = row.cpuIdleId === null ? undefined : row.cpuIdleId;

        const config = {
          // Coloring based Cpu number, same for all machines.
          cpu: cpu.cpu,
          maximumValue: maxCpuFreq,
          freqTrackId,
          idleTrackId,
        };

        const uri = `/cpu_freq_cpu${cpu.ucpu}`;
        const title = `Cpu ${cpu.toString()} Frequency`;
        ctx.tracks.registerTrack({
          uri,
          title,
          tags: {
            kind: CPU_FREQ_TRACK_KIND,
            cpu: cpu.ucpu,
          },
          track: new CpuFreqTrack(config, ctx),
        });
        const trackNode = new TrackNode({uri, title, sortOrder: -40});
        ctx.workspace.addChildInOrder(trackNode);
      }
    }
  }
}
