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

import {THREAD_STATE_TRACK_KIND} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {getThreadUriPrefix, getTrackName} from '../../public/utils';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {createThreadStateTrack} from './thread_state_track';
import {removeFalsyValues} from '../../base/array_utils';
import {TrackNode} from '../../public/workspace';
import {ThreadStateSelectionAggregator} from './thread_state_selection_aggregator';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {createAggregationToTabAdaptor} from '../../components/aggregation_adapter';

import {CPU_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {Engine} from '../../trace_processor/engine';
import ThreadPlugin from '../dev.perfetto.Thread';
import {CpuSliceByProcessSelectionAggregator} from './cpu_slice_by_process_selection_aggregator';
import {CpuSliceSelectionAggregator} from './cpu_slice_selection_aggregator';
import {uriForSchedTrack} from './common';
import {CpuSliceTrack} from './cpu_slice_track';
import {WakerOverlay} from './waker_overlay';

function uriForThreadStateTrack(upid: number | null, utid: number): string {
  return `${getThreadUriPrefix(upid, utid)}_state`;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Sched';
  static readonly dependencies = [ProcessThreadGroupsPlugin, ThreadPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    await this.addCpuSliceTracks(ctx);
    await this.addThreadStateTracks(ctx);
  }

  async addCpuSliceTracks(ctx: Trace): Promise<void> {
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

  private async addThreadStateTracks(ctx: Trace) {
    const {engine} = ctx;

    ctx.selection.registerAreaSelectionTab(
      createAggregationToTabAdaptor(ctx, new ThreadStateSelectionAggregator()),
    );

    const result = await engine.query(`
      include perfetto module viz.threads;
      include perfetto module viz.summary.threads;
      include perfetto module sched.states;

      select
        utid,
        t.upid,
        tid,
        t.name as threadName,
        is_main_thread as isMainThread,
        is_kernel_thread as isKernelThread
      from _threads_with_kernel_flag t
      join _sched_summary using (utid)
    `);

    const it = result.iter({
      utid: NUM,
      upid: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
      isMainThread: NUM_NULL,
      isKernelThread: NUM,
    });
    for (; it.valid(); it.next()) {
      const {utid, upid, tid, threadName, isMainThread, isKernelThread} = it;
      const title = getTrackName({
        utid,
        tid,
        threadName,
        kind: THREAD_STATE_TRACK_KIND,
      });

      const uri = uriForThreadStateTrack(upid, utid);
      ctx.tracks.registerTrack({
        uri,
        title,
        tags: {
          kind: THREAD_STATE_TRACK_KIND,
          utid,
          upid: upid ?? undefined,
          ...(isKernelThread === 1 && {kernelThread: true}),
        },
        chips: removeFalsyValues([
          isKernelThread === 0 && isMainThread === 1 && 'main thread',
        ]),
        track: createThreadStateTrack(ctx, uri, utid),
      });

      const group = ctx.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForThread(utid);
      const track = new TrackNode({uri, title, sortOrder: 10});
      group?.addChildInOrder(track);
    }
  }
}
