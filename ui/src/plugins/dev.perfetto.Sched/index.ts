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
import {ActiveCPUCountTrack, CPUType} from './active_cpu_count';
import {
  RunnableThreadCountTrack,
  UninterruptibleSleepThreadCountTrack,
} from './thread_count';

function uriForThreadStateTrack(upid: number | null, utid: number): string {
  return `${getThreadUriPrefix(upid, utid)}_state`;
}

function uriForActiveCPUCountTrack(cpuType?: CPUType): string {
  const prefix = `/active_cpus`;
  if (cpuType !== undefined) {
    return `${prefix}_${cpuType}`;
  } else {
    return prefix;
  }
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Sched';
  static readonly dependencies = [ProcessThreadGroupsPlugin, ThreadPlugin];

  async onTraceLoad(trace: Trace): Promise<void> {
    await this.addCpuSliceTracks(trace);
    await this.addThreadStateTracks(trace);
    await this.addSchedulingSummaryTracks(trace);
  }

  private async addCpuSliceTracks(trace: Trace): Promise<void> {
    trace.selection.registerAreaSelectionTab(
      createAggregationToTabAdaptor(trace, new CpuSliceSelectionAggregator()),
    );
    trace.selection.registerAreaSelectionTab(
      createAggregationToTabAdaptor(
        trace,
        new CpuSliceByProcessSelectionAggregator(),
      ),
    );

    // ctx.traceInfo.cpus contains all cpus seen from all events. Filter the set
    // if it's seen in sched slices. We order by ucpu to ensure a stable order
    // for the tracks.
    const queryRes = await trace.engine.query(`
      SELECT DISTINCT
        ucpu
      FROM sched
      ORDER BY ucpu
    `);
    const ucpus = new Set<number>();
    for (const it = queryRes.iter({ucpu: NUM}); it.valid(); it.next()) {
      ucpus.add(it.ucpu);
    }
    const cpus = trace.traceInfo.cpus.filter((cpu) => ucpus.has(cpu.ucpu));
    const cpuToClusterType = await this.getAndroidCpuClusterTypes(trace.engine);

    for (const cpu of cpus) {
      const uri = uriForSchedTrack(cpu.ucpu);
      const size = cpuToClusterType.get(cpu.cpu);
      const sizeStr = size === undefined ? `` : ` (${size})`;
      const name = `Cpu ${cpu.cpu}${sizeStr}${cpu.maybeMachineLabel()}`;

      const threads = trace.plugins.getPlugin(ThreadPlugin).getThreadMap();

      trace.tracks.registerTrack({
        uri,
        title: name,
        tags: {
          kind: CPU_SLICE_TRACK_KIND,
          cpu: cpu.ucpu,
        },
        track: new CpuSliceTrack(trace, uri, cpu, threads),
      });
      const trackNode = new TrackNode({uri, title: name, sortOrder: -50});
      trace.workspace.addChildInOrder(trackNode);
    }

    trace.tracks.registerOverlay(new WakerOverlay(trace));
  }

  private async getAndroidCpuClusterTypes(
    engine: Engine,
  ): Promise<Map<number, string>> {
    const cpuToClusterType = new Map<number, string>();
    await engine.query(`
        INCLUDE PERFETTO MODULE android.cpu.cluster_type;
      `);
    const result = await engine.query(`
        SELECT
          cpu,
          cluster_type AS clusterType
        FROM android_cpu_cluster_mapping
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

  private async addThreadStateTracks(trace: Trace) {
    const {engine} = trace;

    trace.selection.registerAreaSelectionTab(
      createAggregationToTabAdaptor(
        trace,
        new ThreadStateSelectionAggregator(),
      ),
    );

    const result = await engine.query(`
      INCLUDE PERFETTO MODULE viz.threads;
      INCLUDE PERFETTO MODULE viz.summary.threads;
      INCLUDE PERFETTO MODULE sched.states;

      SELECT
        utid,
        t.upid,
        tid,
        t.name AS threadName,
        is_main_thread AS isMainThread,
        is_kernel_thread AS isKernelThread
      FROM _threads_with_kernel_flag t
      JOIN _sched_summary USING (utid)
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
      trace.tracks.registerTrack({
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
        track: createThreadStateTrack(trace, uri, utid),
      });

      const group = trace.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForThread(utid);
      const track = new TrackNode({uri, title, sortOrder: 10});
      group?.addChildInOrder(track);
    }
  }

  private addSchedulingSummaryTracks(trace: Trace) {
    const summaryGroup = new TrackNode({title: 'Scheduler', isSummary: true});
    trace.workspace.addChildInOrder(summaryGroup);

    const runnableThreadCountTitle = 'Runnable thread count';
    const runnableThreadCountUri = `/runnable_thread_count`;
    trace.tracks.registerTrack({
      uri: runnableThreadCountUri,
      title: runnableThreadCountTitle,
      track: new RunnableThreadCountTrack(trace, runnableThreadCountUri),
    });
    const runnableThreadCountTrackNode = new TrackNode({
      title: runnableThreadCountTitle,
      uri: runnableThreadCountUri,
    });
    summaryGroup.addChildLast(runnableThreadCountTrackNode);
    // This command only pins the track but the name remains for legacy reasons
    trace.commands.registerCommand({
      id: 'dev.perfetto.Sched.AddRunnableThreadCountTrackCommand',
      name: `Add track: ${runnableThreadCountTitle.toLowerCase()}`,
      callback: () => runnableThreadCountTrackNode.pin(),
    });

    const uninterruptibleSleepThreadCountUri =
      '/uninterruptible_sleep_thread_count';
    const uninterruptibleSleepThreadCountTitle =
      'Uninterruptible Sleep thread count';
    trace.tracks.registerTrack({
      uri: uninterruptibleSleepThreadCountUri,
      title: uninterruptibleSleepThreadCountTitle,
      track: new UninterruptibleSleepThreadCountTrack(
        trace,
        uninterruptibleSleepThreadCountUri,
      ),
    });
    const uninterruptibleSleepThreadCountTrackNode = new TrackNode({
      title: uninterruptibleSleepThreadCountTitle,
      uri: uninterruptibleSleepThreadCountUri,
    });
    summaryGroup.addChildLast(uninterruptibleSleepThreadCountTrackNode);
    trace.commands.registerCommand({
      id: 'dev.perfetto.Sched.AddUninterruptibleSleepThreadCountTrackCommand',
      name: 'Add track: uninterruptible sleep thread count',
      callback: () => uninterruptibleSleepThreadCountTrackNode.pin(),
    });

    const activeCpuCountUri = uriForActiveCPUCountTrack();
    const activeCpuCountTitle = 'Active CPU count';
    trace.tracks.registerTrack({
      uri: activeCpuCountUri,
      title: activeCpuCountTitle,
      track: new ActiveCPUCountTrack({trackUri: activeCpuCountUri}, trace),
    });
    const activeCpuCountTrackNode = new TrackNode({
      title: activeCpuCountTitle,
      uri: activeCpuCountUri,
    });
    summaryGroup.addChildLast(activeCpuCountTrackNode);
    trace.commands.registerCommand({
      id: 'dev.perfetto.Sched.AddActiveCPUCountTrackCommand',
      name: 'Add track: active CPU count',
      callback: () => activeCpuCountTrackNode.pin(),
    });

    for (const cpuType of Object.values(CPUType)) {
      const activeCpuTypeCountUri = uriForActiveCPUCountTrack(cpuType);
      const activeCpuTypeCountTitle = `Active CPU count: ${cpuType}`;
      trace.tracks.registerTrack({
        uri: activeCpuTypeCountUri,
        title: activeCpuTypeCountTitle,
        track: new ActiveCPUCountTrack(
          {trackUri: activeCpuTypeCountUri},
          trace,
          cpuType,
        ),
      });
      const activeCpuTypeCountTrackNode = new TrackNode({
        title: activeCpuTypeCountTitle,
        uri: activeCpuTypeCountUri,
      });
      activeCpuCountTrackNode.addChildLast(activeCpuTypeCountTrackNode);

      trace.commands.registerCommand({
        id: `dev.perfetto.Sched.AddActiveCPUCountTrackCommand.${cpuType}`,
        name: `Add track: active ${cpuType} CPU count`,
        callback: () => activeCpuTypeCountTrackNode.pin(),
      });
    }
  }
}
