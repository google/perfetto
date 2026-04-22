// Copyright (C) 2026 The Android Open Source Project
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
import {assertExists} from '../../base/assert';
import {Icons} from '../../base/semantic_icons';
import {Cpu} from '../../components/cpu';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {NUM, STR} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';
import {TraceProcessorCounterTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_counter_track';

const PROC_STAT_MAN_PAGE =
  'https://man7.org/linux/man-pages/man5/proc_stat.5.html';

interface MetricInfo {
  readonly groupName: string;
  readonly sortOrder: number;
  readonly blurb: string;
}

const METRICS: Record<string, MetricInfo> = {
  user_ns: {
    groupName: 'User Time',
    sortOrder: 0,
    blurb: 'Time spent in user mode.',
  },
  user_nice_ns: {
    groupName: 'Nice Time',
    sortOrder: 1,
    blurb: 'Time spent in user mode with low priority (nice).',
  },
  system_mode_ns: {
    groupName: 'Kernel Time',
    sortOrder: 2,
    blurb: 'Time spent in system (kernel) mode.',
  },
  idle_ns: {
    groupName: 'Idle Time',
    sortOrder: 3,
    blurb:
      'Time spent in the idle task. This value should be USER_HZ times the ' +
      'second entry in the /proc/uptime pseudo-file.',
  },
  io_wait_ns: {
    groupName: 'IO Wait Time',
    sortOrder: 4,
    blurb:
      'Time waiting for I/O to complete. This value is not reliable for ' +
      'reasons documented in the man page.',
  },
  irq_ns: {
    groupName: 'Hardware IRQ Time',
    sortOrder: 5,
    blurb: 'Time servicing hardware interrupts.',
  },
  softirq_ns: {
    groupName: 'Soft IRQ Time',
    sortOrder: 6,
    blurb: 'Time servicing softirqs.',
  },
  steal_ns: {
    groupName: 'Steal Time',
    sortOrder: 7,
    blurb:
      'Stolen time: time spent in other operating systems when running in a ' +
      'virtualized environment.',
  },
};

function renderDescription(blurb: string): m.Children {
  return m('', [
    blurb,
    m('br'),
    'Sourced from /proc/stat. See ',
    m(
      Anchor,
      {
        href: PROC_STAT_MAN_PAGE,
        target: '_blank',
        icon: Icons.ExternalLink,
      },
      'proc_stat(5)',
    ),
    ' for details.',
  ]);
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.CoarseCpu';
  static readonly dependencies = [
    StandardGroupsPlugin,
    TraceProcessorTrackPlugin,
  ];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const result = await ctx.engine.query(`
      include perfetto module viz.summary.counters;

      select
        ct.id as trackId,
        extract_arg(ct.dimension_arg_set_id, 'cpu') as cpu,
        extract_arg(ct.dimension_arg_set_id, 'cpustat_key') as metric,
        ct.machine_id as machineId,
        ifnull(cpu.ucpu, extract_arg(ct.dimension_arg_set_id, 'cpu')) as ucpu
      from counter_track ct
      join _counter_track_summary using (id)
      left join cpu
        on cpu.cpu = extract_arg(ct.dimension_arg_set_id, 'cpu')
       and cpu.machine_id = ct.machine_id
      where ct.type = 'cpustat'
      order by metric, ucpu
    `);

    const it = result.iter({
      trackId: NUM,
      cpu: NUM,
      metric: STR,
      machineId: NUM,
      ucpu: NUM,
    });
    if (!it.valid()) return;

    const cpuStandardGroup = ctx.plugins
      .getPlugin(StandardGroupsPlugin)
      .getOrCreateStandardGroup(ctx.defaultWorkspace, 'CPU');

    const metricGroups = new Map<string, TrackNode>();

    for (; it.valid(); it.next()) {
      const {trackId, cpu, metric, machineId, ucpu} = it;

      const info = assertExists(METRICS[metric]);

      let metricGroup = metricGroups.get(metric);
      if (metricGroup === undefined) {
        metricGroup = new TrackNode({
          name: info.groupName,
          isSummary: true,
          sortOrder: info.sortOrder,
        });
        metricGroups.set(metric, metricGroup);
        cpuStandardGroup.addChildInOrder(metricGroup);
      }

      const trackName = `${info.groupName} (CPU ${new Cpu(ucpu, cpu, machineId).toString()})`;
      const uri = `/coarse_cpu_${trackId}`;

      ctx.tracks.registerTrack({
        uri,
        description: () => renderDescription(info.blurb),
        tags: {
          kinds: [COUNTER_TRACK_KIND],
          trackIds: [trackId],
          type: 'cpustat',
          cpu: ucpu,
        },
        renderer: new TraceProcessorCounterTrack({
          trace: ctx,
          uri,
          trackId,
          trackName,
          yMode: 'delta',
        }),
      });

      metricGroup.addChildInOrder(
        new TrackNode({
          uri,
          name: trackName,
        }),
      );
    }
  }
}
