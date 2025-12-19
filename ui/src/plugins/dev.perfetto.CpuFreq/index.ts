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

import m from 'mithril';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {NUM, NUM_NULL} from '../../trace_processor/query_result';
import {CpuFreqTrack} from './cpu_freq_track';
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';
import {Cpu} from '../../components/cpu';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.CpuFreq';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const {engine} = ctx;

    // Find the list of CPU frequency track ids and their corresponding CPU idle
    // track ids if they exist
    const tracksResult = await engine.query(`
      SELECT
        track.id AS freqTrackId,
        t2.id AS idleTrackId,
        cpu.ucpu AS ucpu,
        IFNULL(track.machine_id, 0) AS machineId,
        track.cpu AS cpu
      FROM cpu_counter_track track
      JOIN cpu
        ON track.cpu = cpu.cpu
       AND IFNULL(track.machine_id, 0) = IFNULL(cpu.machine_id, 0)
      LEFT JOIN cpu_counter_track t2
        ON track.cpu = t2.cpu
       AND IFNULL(track.machine_id, 0) = IFNULL(t2.machine_id, 0)
       AND t2.type = 'cpu_idle'
      WHERE
        track.type = 'cpu_frequency'
      ORDER BY ucpu
    `);

    // Find the maximum CPU frequency across all CPUs to set the graph scale
    const maxCpuFreqResult = await engine.query(`
      SELECT
        IFNULL(MAX(value), 0) AS freq
      FROM counter c
      JOIN cpu_counter_track t ON c.track_id = t.id
      JOIN _counter_track_summary s ON t.id = s.id
      WHERE t.type = 'cpu_frequency';
    `);
    const maxCpuFreq = maxCpuFreqResult.firstRow({freq: NUM}).freq;

    const group = new TrackNode({
      name: 'CPU Frequency',
      sortOrder: -40,
      isSummary: true,
      collapsed: false,
    });

    for (
      const it = tracksResult.iter({
        freqTrackId: NUM,
        machineId: NUM,
        cpu: NUM,
        ucpu: NUM,
        idleTrackId: NUM_NULL,
      });
      it.valid();
      it.next()
    ) {
      const {freqTrackId, idleTrackId, machineId, cpu, ucpu} = it;
      const uri = `/cpu_freq_cpu${ucpu}`;

      ctx.tracks.registerTrack({
        uri,
        tags: {
          cpu: ucpu,
        },
        renderer: new CpuFreqTrack(
          {
            // Coloring based Cpu number, same for all machines.
            cpu,
            maximumValue: maxCpuFreq,
            freqTrackId,
            idleTrackId: idleTrackId ?? undefined,
          },
          ctx,
        ),
        description: () => {
          return m('', [
            `Shows the CPU frequency ${cpu.toString()} over time.`,
            m('br'),
            m(
              Anchor,
              {
                href: 'https://perfetto.dev/docs/data-sources/cpu-freq',
                target: '_blank',
                icon: Icons.ExternalLink,
              },
              'Documentation',
            ),
          ]);
        },
      });

      const trackNode = new TrackNode({
        uri,
        name: `CPU ${new Cpu(ucpu, cpu, machineId).toString()} Frequency`,
      });

      group.addChildInOrder(trackNode);
    }

    if (group.children.length > 0) {
      ctx.defaultWorkspace.addChildInOrder(group);
    }
  }
}
