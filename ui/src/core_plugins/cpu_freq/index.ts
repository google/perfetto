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

import {TrackNode} from '../../public/workspace';
import {CPU_FREQ_TRACK_KIND} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {NUM, NUM_NULL} from '../../trace_processor/query_result';
import {CpuFreqTrack} from './cpu_freq_track';

class CpuFreq implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    const {engine} = ctx;

    const cpus = [];
    const cpusResult = await engine.query(
      'select distinct cpu from cpu_counter_track order by cpu;',
    );
    for (const it = cpusResult.iter({cpu: NUM}); it.valid(); it.next()) {
      cpus.push(it.cpu);
    }

    const maxCpuFreqResult = await engine.query(`
      select ifnull(max(value), 0) as freq
      from counter c
      join cpu_counter_track t on c.track_id = t.id
      join _counter_track_summary s on t.id = s.id
      where name = 'cpufreq';
    `);
    const maxCpuFreq = maxCpuFreqResult.firstRow({freq: NUM}).freq;

    for (const cpu of cpus) {
      // Only add a cpu freq track if we have cpu freq data.
      const cpuFreqIdleResult = await engine.query(`
        select
          id as cpuFreqId,
          (
            select id
            from cpu_counter_track
            where name = 'cpuidle'
            and cpu = ${cpu}
            limit 1
          ) as cpuIdleId
        from cpu_counter_track
        join _counter_track_summary using (id)
        where name = 'cpufreq' and cpu = ${cpu}
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
          cpu,
          maximumValue: maxCpuFreq,
          freqTrackId,
          idleTrackId,
        };

        const uri = `/cpu_freq_cpu${cpu}`;
        const title = `Cpu ${cpu} Frequency`;
        ctx.tracks.registerTrack({
          uri,
          title,
          tags: {
            kind: CPU_FREQ_TRACK_KIND,
            cpu,
          },
          track: new CpuFreqTrack(config, ctx.engine),
        });
        const trackNode = new TrackNode({uri, title, sortOrder: -40});
        ctx.workspace.addChildInOrder(trackNode);
      }
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.CpuFreq',
  plugin: CpuFreq,
};
