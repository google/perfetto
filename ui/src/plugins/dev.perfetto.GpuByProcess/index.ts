// Copyright (C) 2023 The Android Open Source Project
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

import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {TrackNode} from '../../public/workspace';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {ThreadSliceDetailsPanel} from '../../components/details/thread_slice_details_tab';
import {Gpu} from '../../components/gpu';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';

function getProcessDisplayName(
  name: string | null,
  pid: number | null,
): string {
  if (name != null) {
    return name;
  } else if (pid != null) {
    return `Process ${pid}`;
  }
  return 'Unknown';
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.GpuByProcess';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    // One sub-track per (process, hw_queue). hw_queue_id is per-sequence so
    // different processes can reuse the same value for distinct queues; the
    // (upid, hw_queue_id) tuple disambiguates them. The sub-track name is
    // taken from the corresponding global hw queue track so it matches the
    // existing naming under the GPU section.
    //
    // When a process has events on multiple GPUs, hw queue tracks are nested
    // one more level under per-GPU groups ("GPU 0", "GPU 1", ...) to mirror
    // the layout used by dev.perfetto.Gpu under the global GPU section.
    const results = await ctx.engine.query(`
      SELECT
        s.upid AS upid,
        s.hw_queue_id AS hw_queue_id,
        MIN(t.name) AS track_name,
        extract_arg(t.dimension_arg_set_id, 'ugpu') AS ugpu,
        extract_arg(t.dimension_arg_set_id, 'gpu') AS gpu_id,
        t.machine_id AS machine_id,
        g.name AS gpu_name,
        p.pid AS pid,
        p.name AS process_name
      FROM gpu_slice s
      JOIN gpu_track t ON s.track_id = t.id
      JOIN process p USING (upid)
      LEFT JOIN gpu g ON extract_arg(t.dimension_arg_set_id, 'ugpu') = g.id
      WHERE s.upid IS NOT NULL AND s.hw_queue_id IS NOT NULL
      GROUP BY s.upid, s.hw_queue_id
      ORDER BY s.upid, ugpu, s.hw_queue_id
    `);

    const it = results.iter({
      upid: NUM,
      hw_queue_id: NUM,
      track_name: STR,
      ugpu: NUM_NULL,
      gpu_id: NUM_NULL,
      machine_id: NUM,
      gpu_name: STR_NULL,
      pid: NUM_NULL,
      process_name: STR_NULL,
    });

    interface Row {
      upid: number;
      hwqId: number;
      trackName: string;
      gpu: Gpu | null;
      pid: number | null;
      processName: string | null;
    }

    // First pass: collect rows and count distinct ugpus per process.
    const rows: Row[] = [];
    const ugpusByUpid = new Map<number, Set<number>>();
    for (; it.valid(); it.next()) {
      const gpu =
        it.gpu_id !== null
          ? new Gpu(
              it.ugpu ?? it.gpu_id,
              it.gpu_id,
              it.machine_id,
              it.gpu_name ?? undefined,
            )
          : null;
      rows.push({
        upid: it.upid,
        hwqId: it.hw_queue_id,
        trackName: it.track_name,
        gpu,
        pid: it.pid,
        processName: it.process_name,
      });
      if (gpu !== null) {
        let set = ugpusByUpid.get(it.upid);
        if (set === undefined) {
          set = new Set<number>();
          ugpusByUpid.set(it.upid, set);
        }
        set.add(gpu.ugpu);
      }
    }

    const processGroups = ctx.plugins.getPlugin(ProcessThreadGroupsPlugin);
    const gpuGroupByUpid = new Map<number, TrackNode>();
    // Per-(upid, ugpu) sub-group; only created when the process spans multiple
    // GPUs.
    const perGpuGroupByUpidUgpu = new Map<string, TrackNode>();

    for (const row of rows) {
      const {upid, hwqId, trackName, gpu} = row;
      const uri = `dev.perfetto.GpuByProcess#${upid}#${hwqId}`;
      ctx.tracks.registerTrack({
        uri,
        renderer: SliceTrack.create({
          trace: ctx,
          uri,
          dataset: new SourceDataset({
            src: `(SELECT id, name, ts, dur, depth FROM gpu_slice WHERE upid = ${upid} AND hw_queue_id = ${hwqId})`,
            schema: {
              id: NUM,
              name: STR,
              ts: LONG,
              dur: LONG,
              depth: NUM,
            },
          }),
          detailsPanel: () => new ThreadSliceDetailsPanel(ctx),
        }),
      });

      let processGroup = processGroups.getGroupForProcess(upid);
      if (processGroup === undefined) {
        const displayName = getProcessDisplayName(row.processName, row.pid);
        processGroup = new TrackNode({
          uri: `/process_${upid}`,
          name: `${displayName} ${row.pid ?? upid}`,
          isSummary: true,
          sortOrder: 50,
        });
        ctx.defaultWorkspace.addChildInOrder(processGroup);
      }

      let gpuGroup = gpuGroupByUpid.get(upid);
      if (gpuGroup === undefined) {
        gpuGroup = new TrackNode({
          uri: `dev.perfetto.GpuByProcess#${upid}`,
          name: 'GPU',
          isSummary: true,
          sortOrder: -50,
        });
        processGroup.addChildInOrder(gpuGroup);
        gpuGroupByUpid.set(upid, gpuGroup);
      }

      // If the process spans multiple GPUs, nest hw queue tracks under a
      // per-GPU sub-group. Otherwise add them directly under the GPU group.
      let parent = gpuGroup;
      const distinctGpus = ugpusByUpid.get(upid)?.size ?? 0;
      if (gpu !== null && distinctGpus > 1) {
        const key = `${upid}#${gpu.ugpu}`;
        let perGpu = perGpuGroupByUpidUgpu.get(key);
        if (perGpu === undefined) {
          perGpu = new TrackNode({
            uri: `dev.perfetto.GpuByProcess#${upid}#gpu_${gpu.ugpu}`,
            name: `${gpu.displayName}${gpu.maybeMachineLabel()}`,
            isSummary: true,
            sortOrder: gpu.sortOrder,
          });
          gpuGroup.addChildInOrder(perGpu);
          perGpuGroupByUpidUgpu.set(key, perGpu);
        }
        parent = perGpu;
      }

      parent.addChildInOrder(
        new TrackNode({
          uri,
          name: trackName,
          sortOrder: hwqId,
        }),
      );
    }
  }
}
