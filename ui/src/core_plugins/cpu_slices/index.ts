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

import {CPU_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {SchedSliceDetailsPanel} from './sched_details_tab';
import {Engine} from '../../trace_processor/engine';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {NUM, STR_NULL} from '../../trace_processor/query_result';
import {CpuSliceTrack} from './cpu_slice_track';
import {TrackNode} from '../../public/workspace';
import {CpuSliceSelectionAggregator} from './cpu_slice_selection_aggregator';
import {CpuSliceByProcessSelectionAggregator} from './cpu_slice_by_process_selection_aggregator';

function uriForSchedTrack(cpu: number): string {
  return `/sched_cpu${cpu}`;
}

class CpuSlices implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.selection.registerAreaSelectionAggreagtor(
      new CpuSliceSelectionAggregator(),
    );
    ctx.selection.registerAreaSelectionAggreagtor(
      new CpuSliceByProcessSelectionAggregator(),
    );

    const cpus = ctx.traceInfo.cpus;
    const cpuToClusterType = await this.getAndroidCpuClusterTypes(ctx.engine);

    for (const cpu of cpus) {
      const size = cpuToClusterType.get(cpu);
      const uri = uriForSchedTrack(cpu);

      const name = size === undefined ? `Cpu ${cpu}` : `Cpu ${cpu} (${size})`;
      ctx.tracks.registerTrack({
        uri,
        title: name,
        tags: {
          kind: CPU_SLICE_TRACK_KIND,
          cpu,
        },
        track: new CpuSliceTrack(ctx, uri, cpu),
        detailsPanel: () => new SchedSliceDetailsPanel(ctx),
      });
      const trackNode = new TrackNode({uri, title: name, sortOrder: -50});
      ctx.workspace.addChildInOrder(trackNode);
    }

    ctx.selection.registerSqlSelectionResolver({
      sqlTableName: 'sched_slice',
      callback: async (id: number) => {
        const result = await ctx.engine.query(`
          select
            cpu
          from sched_slice
          where id = ${id}
        `);

        const cpu = result.firstRow({
          cpu: NUM,
        }).cpu;

        return {
          eventId: id,
          trackUri: uriForSchedTrack(cpu),
        };
      },
    });
  }

  async getAndroidCpuClusterTypes(
    engine: Engine,
  ): Promise<Map<number, string>> {
    const cpuToClusterType = new Map<number, string>();
    await engine.query(`
      include perfetto module android.cpu.cluster_type;
    `);
    const result = await engine.query(`
      select cpu, cluster_type as clusterType
      from android_cpu_cluster_mapping
    `);

    const it = result.iter({
      cpu: NUM,
      clusterType: STR_NULL,
    });

    for (; it.valid(); it.next()) {
      const clusterType = it.clusterType;
      if (clusterType !== null) {
        cpuToClusterType.set(it.cpu, clusterType);
      }
    }

    return cpuToClusterType;
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.CpuSlices',
  plugin: CpuSlices,
};
