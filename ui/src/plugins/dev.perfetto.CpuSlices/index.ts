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

import {createAggregationToTabAdaptor} from '../../components/aggregation_adapter';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {CPU_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import {Engine} from '../../trace_processor/engine';
import {NUM, STR_NULL} from '../../trace_processor/query_result';
import ThreadPlugin from '../dev.perfetto.Thread';
import {uriForSchedTrack} from './common';
import {CpuSliceByProcessSelectionAggregator} from './cpu_slice_by_process_selection_aggregator';
import {CpuSliceSelectionAggregator} from './cpu_slice_selection_aggregator';
import {CpuSliceTrack} from './cpu_slice_track';
import {WakerOverlay} from './waker_overlay';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.CpuSlices';
  static readonly dependencies = [ThreadPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.selection.registerAreaSelectionTab(
      createAggregationToTabAdaptor(ctx, new CpuSliceSelectionAggregator()),
    );
    ctx.selection.registerAreaSelectionTab(
      createAggregationToTabAdaptor(
        ctx,
        new CpuSliceByProcessSelectionAggregator(),
      ),
    );

    // ctx.traceInfo.cpus contains all cpus seen from all events. Filter the set
    // if it's seen in sched slices.
    const queryRes = await ctx.engine.query(
      `select distinct ucpu from sched order by ucpu;`,
    );
    const ucpus = new Set<number>();
    for (const it = queryRes.iter({ucpu: NUM}); it.valid(); it.next()) {
      ucpus.add(it.ucpu);
    }
    const cpus = ctx.traceInfo.cpus.filter((cpu) => ucpus.has(cpu.ucpu));
    const cpuToClusterType = await this.getAndroidCpuClusterTypes(ctx.engine);

    for (const cpu of cpus) {
      const uri = uriForSchedTrack(cpu.ucpu);
      const size = cpuToClusterType.get(cpu.cpu);
      const sizeStr = size === undefined ? `` : ` (${size})`;
      const name = `Cpu ${cpu.cpu}${sizeStr}${cpu.maybeMachineLabel()}`;

      const threads = ctx.plugins.getPlugin(ThreadPlugin).getThreadMap();

      ctx.tracks.registerTrack({
        uri,
        title: name,
        tags: {
          kind: CPU_SLICE_TRACK_KIND,
          cpu: cpu.ucpu,
        },
        track: new CpuSliceTrack(ctx, uri, cpu, threads),
      });
      const trackNode = new TrackNode({uri, title: name, sortOrder: -50});
      ctx.workspace.addChildInOrder(trackNode);
    }

    ctx.tracks.registerOverlay(new WakerOverlay(ctx));
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
