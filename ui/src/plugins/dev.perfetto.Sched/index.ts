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
import {
  LONG,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
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
import {duration, time, Time} from '../../base/time';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {MinimapRow} from '../../public/minimap';
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

  async onTraceLoad(ctx: Trace): Promise<void> {
    await this.addCpuSliceTracks(ctx);
    await this.addThreadStateTracks(ctx);
    await this.addMinimapProvider(ctx);
    await this.addSchedulingSummaryTracks(ctx);

    ctx.commands.registerCommand({
      id: 'dev.perfetto.Sched#SelectAllThreadStateTracks',
      name: 'Select all thread state tracks',
      callback: () => {
        const tracks = ctx.tracks
          .getAllTracks()
          .filter((t) => t.tags?.kind === THREAD_STATE_TRACK_KIND);
        ctx.selection.selectArea({
          trackUris: tracks.map((t) => t.uri),
          start: ctx.traceInfo.start,
          end: ctx.traceInfo.end,
        });
      },
    });
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
        tags: {
          kind: CPU_SLICE_TRACK_KIND,
          cpu: cpu.ucpu,
        },
        renderer: new CpuSliceTrack(ctx, uri, cpu, threads),
      });
      const trackNode = new TrackNode({uri, name, sortOrder: -50});
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

  private async getCpus(engine: Engine): Promise<number[]> {
    const result = await engine.query(`
      SELECT DISTINCT
        ucpu
      FROM sched
    `);
    const it = result.iter({ucpu: NUM});
    const cpus: number[] = [];
    for (; it.valid(); it.next()) {
      cpus.push(it.ucpu);
    }
    return cpus;
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
        tags: {
          kind: THREAD_STATE_TRACK_KIND,
          utid,
          upid: upid ?? undefined,
          ...(isKernelThread === 1 && {kernelThread: true}),
        },
        chips: removeFalsyValues([
          isKernelThread === 0 && isMainThread === 1 && 'main thread',
        ]),
        renderer: createThreadStateTrack(ctx, uri, utid),
      });

      const group = ctx.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForThread(utid);
      const track = new TrackNode({uri, name: title, sortOrder: 10});
      group?.addChildInOrder(track);
    }
  }

  private async addMinimapProvider(trace: Trace) {
    const hasSched = await this.hasSched(trace.engine);
    if (!hasSched) {
      return;
    }

    trace.minimap.registerContentProvider({
      priority: 2, // Higher priority than the default slices minimap
      getData: async (_, resolution) => {
        const start = trace.traceInfo.start;
        const end = trace.traceInfo.end;
        const cpus = await this.getCpus(trace.engine);
        const rows: MinimapRow[] = [];

        const intervals: bigint[] = [];
        for (let i: bigint = start; i < end; i += resolution) {
          intervals.push(i);
        }

        const values = intervals
          .map((ts, index) => `(${index}, ${ts}, ${resolution})`)
          .join();

        const intervalsTableName = '__minimap_sched_intervals';

        await trace.engine.query(`
          CREATE TABLE ${intervalsTableName} (
            id INTEGER PRIMARY KEY,
            ts INTEGER,
            dur INTEGER
          );

          INSERT INTO ${intervalsTableName} (id, ts, dur)
          values ${values}
        `);

        for (const cpu of cpus) {
          // TODO(stevegolton): Obtain source data from the track's datasets
          // instead of repeating it here?
          const schedTableName = '__sched_per_cpu';
          await using _schedTable = await createPerfettoTable(
            trace.engine,
            schedTableName,
            `
              SELECT
                *
              FROM sched
              WHERE
                dur > 0 AND
                ucpu = ${cpu} AND
                NOT utid IN (SELECT utid FROM thread WHERE is_idle)
            `,
          );

          const entireQuery = `
            SELECT
              id_1 AS bucketId,
              CAST(SUM(ii.dur) AS FLOAT)/${resolution} AS load,
              intervals.ts AS ts,
              intervals.dur AS dur
            FROM _interval_intersect!((${schedTableName}, ${intervalsTableName}), ()) ii
            JOIN ${intervalsTableName} intervals ON (id_1 = intervals.id)
            GROUP BY id_1;
          `;

          const results = await trace.engine.query(entireQuery);
          const iter = results.iter({
            load: NUM,
            ts: LONG,
            dur: LONG,
          });

          const loads: {ts: time; load: number; dur: duration}[] = [];
          for (; iter.valid(); iter.next()) {
            loads.push({
              load: iter.load,
              ts: Time.fromRaw(iter.ts),
              dur: iter.dur,
            });
          }

          rows.push(loads);
        }

        return rows;
      },
    });
  }

  private async hasSched(engine: Engine): Promise<boolean> {
    const result = await engine.query(`SELECT ts FROM sched LIMIT 1`);
    return result.numRows() > 0;
  }

  private addSchedulingSummaryTracks(ctx: Trace) {
    const summaryGroup = new TrackNode({name: 'Scheduler', isSummary: true});
    ctx.workspace.addChildInOrder(summaryGroup);

    const runnableThreadCountTitle = 'Runnable thread count';
    const runnableThreadCountUri = `/runnable_thread_count`;
    ctx.tracks.registerTrack({
      uri: runnableThreadCountUri,
      renderer: new RunnableThreadCountTrack(ctx, runnableThreadCountUri),
    });
    const runnableThreadCountTrackNode = new TrackNode({
      name: runnableThreadCountTitle,
      uri: runnableThreadCountUri,
    });
    summaryGroup.addChildLast(runnableThreadCountTrackNode);
    // This command only pins the track but the name remains for legacy reasons
    ctx.commands.registerCommand({
      id: 'dev.perfetto.Sched.AddRunnableThreadCountTrackCommand',
      name: `Add track: ${runnableThreadCountTitle.toLowerCase()}`,
      callback: () => runnableThreadCountTrackNode.pin(),
    });

    const uninterruptibleSleepThreadCountUri =
      '/uninterruptible_sleep_thread_count';
    const uninterruptibleSleepThreadCountTitle =
      'Uninterruptible Sleep thread count';
    ctx.tracks.registerTrack({
      uri: uninterruptibleSleepThreadCountUri,
      renderer: new UninterruptibleSleepThreadCountTrack(
        ctx,
        uninterruptibleSleepThreadCountUri,
      ),
    });
    const uninterruptibleSleepThreadCountTrackNode = new TrackNode({
      name: uninterruptibleSleepThreadCountTitle,
      uri: uninterruptibleSleepThreadCountUri,
    });
    summaryGroup.addChildLast(uninterruptibleSleepThreadCountTrackNode);
    ctx.commands.registerCommand({
      id: 'dev.perfetto.Sched.AddUninterruptibleSleepThreadCountTrackCommand',
      name: 'Add track: uninterruptible sleep thread count',
      callback: () => uninterruptibleSleepThreadCountTrackNode.pin(),
    });

    const activeCpuCountUri = uriForActiveCPUCountTrack();
    const activeCpuCountTitle = 'Active CPU count';
    ctx.tracks.registerTrack({
      uri: activeCpuCountUri,
      renderer: new ActiveCPUCountTrack({trackUri: activeCpuCountUri}, ctx),
    });
    const activeCpuCountTrackNode = new TrackNode({
      name: activeCpuCountTitle,
      uri: activeCpuCountUri,
    });
    summaryGroup.addChildLast(activeCpuCountTrackNode);
    ctx.commands.registerCommand({
      id: 'dev.perfetto.Sched.AddActiveCPUCountTrackCommand',
      name: 'Add track: active CPU count',
      callback: () => activeCpuCountTrackNode.pin(),
    });

    for (const cpuType of Object.values(CPUType)) {
      const activeCpuTypeCountUri = uriForActiveCPUCountTrack(cpuType);
      const activeCpuTypeCountTitle = `Active CPU count: ${cpuType}`;
      ctx.tracks.registerTrack({
        uri: activeCpuTypeCountUri,
        renderer: new ActiveCPUCountTrack(
          {trackUri: activeCpuTypeCountUri},
          ctx,
          cpuType,
        ),
      });
      const activeCpuTypeCountTrackNode = new TrackNode({
        name: activeCpuTypeCountTitle,
        uri: activeCpuTypeCountUri,
      });
      activeCpuCountTrackNode.addChildLast(activeCpuTypeCountTrackNode);

      ctx.commands.registerCommand({
        id: `dev.perfetto.Sched.AddActiveCPUCountTrackCommand.${cpuType}`,
        name: `Add track: active ${cpuType} CPU count`,
        callback: () => activeCpuTypeCountTrackNode.pin(),
      });
    }
  }
}
