// Copyright (C) 2025 The Android Open Source Project
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

import {Gpu} from '../../components/gpu';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {COUNTER_TRACK_KIND, SLICE_TRACK_KIND} from '../../public/track_kinds';
import {getTrackName} from '../../public/utils';
import {TrackNode} from '../../public/workspace';
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';
import {TraceProcessorCounterTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_counter_track';
import {createTraceProcessorSliceTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_slice_track';

interface GpuCounterSchema {
  readonly type: string;
  readonly group: string | undefined;
}

interface GpuSliceSchema {
  readonly type: string;
  readonly group: string | undefined;
}

const GPU_COUNTER_SCHEMAS: ReadonlyArray<GpuCounterSchema> = [
  {type: 'gpu_counter', group: 'Counters'},
  {type: 'gpu_memory', group: undefined},
  {type: 'virtgpu_latency', group: 'Virtgpu Latency'},
  {type: 'virtgpu_num_free', group: 'Virtgpu num_free'},
  {type: 'vulkan_device_mem_allocation', group: 'Vulkan Allocations'},
  {type: 'vulkan_device_mem_bind', group: 'Vulkan Binds'},
  {type: 'vulkan_driver_mem', group: 'Vulkan Driver Memory'},
];

const GPU_SLICE_SCHEMAS: ReadonlyArray<GpuSliceSchema> = [
  {type: 'mali_mcu_state', group: undefined},
  {type: 'virtgpu_queue_event', group: 'Virtio GPU Events'},
  {type: 'gpu_render_stage', group: 'Hardware Queues'},
  {type: 'vulkan_events', group: undefined},
  {type: 'gpu_log', group: undefined},
  {type: 'graphics_frame_event', group: undefined},
];

export default class GpuPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Gpu';
  static readonly dependencies = [
    StandardGroupsPlugin,
    TraceProcessorTrackPlugin,
  ];

  private groups = new Map<string, TrackNode>();
  private gpuCount = 0;

  async onTraceLoad(ctx: Trace): Promise<void> {
    const gpuCountResult = await ctx.engine.query(`
      select count(*) as cnt from gpu
    `);
    this.gpuCount = gpuCountResult.firstRow({cnt: NUM}).cnt;

    await this.addGpuFreq(ctx);
    await this.addCounters(ctx);
    await this.addSlices(ctx);
  }

  private async addGpuFreq(ctx: Trace) {
    const result = await ctx.engine.query(`
      select
        gct.id,
        gct.gpu_id as gpuId,
        gct.unit,
        gct.machine_id as machineId,
        gct.ugpu
      from gpu_counter_track gct
      join _counter_track_summary using (id)
      where gct.name = 'gpufreq'
      order by machineId, ugpu
    `);

    const tracks: Array<{
      id: number;
      gpuId: number;
      unit: string | null;
      gpu: Gpu;
    }> = [];
    const it = result.iter({
      id: NUM,
      gpuId: NUM,
      unit: STR_NULL,
      machineId: NUM,
      ugpu: NUM_NULL,
    });
    for (; it.valid(); it.next()) {
      tracks.push({
        id: it.id,
        gpuId: it.gpuId,
        unit: it.unit,
        gpu: new Gpu(it.ugpu ?? it.gpuId, it.gpuId, it.machineId),
      });
    }

    if (tracks.length === 0) return;

    const gpuGroup = this.getGpuGroup(ctx);

    // Only create a sub-group if there's more than one track.
    let parent: TrackNode;
    if (tracks.length > 1) {
      parent = this.getGroupByName(gpuGroup, 'GPU Frequency', null);
    } else {
      parent = gpuGroup;
    }

    for (const {id, gpuId, unit, gpu} of tracks) {
      const uri = `/gpu_frequency_${gpu.ugpu}`;
      const name = `Gpu ${gpuId} Frequency${gpu.maybeMachineLabel()}`;
      ctx.tracks.registerTrack({
        uri,
        tags: {
          kinds: [COUNTER_TRACK_KIND],
          trackIds: [id],
        },
        renderer: new TraceProcessorCounterTrack(
          ctx,
          uri,
          {unit: unit ?? undefined},
          id,
          name,
        ),
      });
      parent.addChildInOrder(new TrackNode({uri, name, sortOrder: -20}));
    }
  }

  private getGpuGroup(ctx: Trace): TrackNode {
    return ctx.plugins
      .getPlugin(StandardGroupsPlugin)
      .getOrCreateStandardGroup(ctx.defaultWorkspace, 'GPU');
  }

  private addToGpuGroup(
    ctx: Trace,
    group: string | undefined,
    gpu: Gpu | null,
    track: TrackNode,
  ) {
    const gpuGroup = this.getGpuGroup(ctx);

    if (gpu !== null && group !== undefined && this.gpuCount > 1) {
      const parentGroup = this.getGroupByName(gpuGroup, group, null);
      const gpuSubGroupName = `GPU ${gpu.gpu} ${group}${gpu.maybeMachineLabel()}`;
      const scopeId =
        gpu.machine > 0 ? `${gpu.gpu}_m${gpu.machine}` : `${gpu.gpu}`;
      const gpuSubGroup = this.getGroupByName(
        parentGroup,
        gpuSubGroupName,
        scopeId,
      );
      gpuSubGroup.addChildInOrder(track);
    } else {
      this.getGroupByName(gpuGroup, group, null).addChildInOrder(track);
    }
  }

  private getGroupByName(
    node: TrackNode,
    group: string | undefined,
    scopeId: string | null,
  ): TrackNode {
    if (group === undefined) {
      return node;
    }
    const groupId = `gpu_group_${scopeId}_${group.toLowerCase().replace(' ', '_')}`;
    const groupNode = this.groups.get(groupId);
    if (groupNode) {
      return groupNode;
    }
    const newGroup = new TrackNode({
      uri: `/${group}`,
      isSummary: true,
      name: group,
      collapsed: true,
    });
    node.addChildInOrder(newGroup);
    this.groups.set(groupId, newGroup);
    return newGroup;
  }

  private async addCounters(ctx: Trace) {
    const counterTypes = GPU_COUNTER_SCHEMAS.map((s) => `'${s.type}'`).join(
      ',',
    );
    const result = await ctx.engine.query(`
      with tracks_summary as (
        select
          ct.type,
          ct.name,
          ct.id,
          ct.unit,
          ct.machine_id,
          extract_arg(ct.dimension_arg_set_id, 'ugpu') as ugpu,
          extract_arg(ct.dimension_arg_set_id, 'gpu') as gpu_id,
          extract_arg(ct.source_arg_set_id, 'description') as description
        from counter_track ct
        join _counter_track_summary using (id)
        where ct.type in (${counterTypes})
        order by ct.name
      )
      select * from tracks_summary
      order by lower(name)
    `);

    const schemas = new Map(GPU_COUNTER_SCHEMAS.map((x) => [x.type, x]));
    const it = result.iter({
      id: NUM,
      type: STR,
      name: STR_NULL,
      unit: STR_NULL,
      gpu_id: NUM_NULL,
      machine_id: NUM,
      description: STR_NULL,
      ugpu: NUM_NULL,
    });
    for (; it.valid(); it.next()) {
      const {
        type,
        id: trackId,
        name,
        unit,
        gpu_id: gpuId,
        machine_id: machineId,
        description,
        ugpu,
      } = it;
      const schema = schemas.get(type);
      if (schema === undefined) {
        continue;
      }
      const gpu =
        gpuId !== null ? new Gpu(ugpu ?? trackId, gpuId, machineId) : null;
      const trackName = getTrackName({name, kind: COUNTER_TRACK_KIND});
      const uri = `/counter_${ugpu ?? trackId}_${trackId}`;

      ctx.tracks.registerTrack({
        uri,
        description: description ?? undefined,
        tags: {
          kinds: [COUNTER_TRACK_KIND],
          trackIds: [trackId],
          type,
          name: name ?? undefined,
          unit: unit ?? undefined,
          description: description ?? undefined,
        },
        renderer: new TraceProcessorCounterTrack(
          ctx,
          uri,
          {unit: unit ?? undefined},
          trackId,
          trackName,
        ),
      });
      this.addToGpuGroup(
        ctx,
        schema.group,
        gpu,
        new TrackNode({uri, name: trackName, sortOrder: 0}),
      );
    }
  }

  private async addSlices(ctx: Trace) {
    const sliceTypes = GPU_SLICE_SCHEMAS.map((s) => `'${s.type}'`).join(',');

    await using _ = await createPerfettoTable({
      name: '__gpu_tracks_to_create',
      engine: ctx.engine,
      as: `
        with grouped as materialized (
          select
            t.type,
            min(t.name) as name,
            lower(min(t.name)) as lower_name,
            extract_arg(t.dimension_arg_set_id, 'ugpu') as ugpu,
            t.machine_id,
            extract_arg(t.source_arg_set_id, 'description') as description,
            min(t.id) minTrackId,
            group_concat(t.id) as trackIds,
            count() as trackCount,
            __max_layout_depth(count(), group_concat(t.id)) as maxDepth,
            extract_arg(t.dimension_arg_set_id, 'gpu') as gpu_id
          from _slice_track_summary s
          join track t using (id)
          where t.type in (${sliceTypes})
          group by type, t.track_group_id, ifnull(t.track_group_id, t.id),
            extract_arg(t.dimension_arg_set_id, 'ugpu')
        )
        select * from grouped
        order by lower_name
      `,
    });

    const result = await ctx.engine.query(
      'select * from __gpu_tracks_to_create',
    );

    const schemas = new Map(GPU_SLICE_SCHEMAS.map((x) => [x.type, x]));
    const it = result.iter({
      type: STR,
      name: STR_NULL,
      gpu_id: NUM_NULL,
      machine_id: NUM,
      trackIds: STR,
      maxDepth: NUM,
      description: STR_NULL,
      ugpu: NUM_NULL,
    });
    for (; it.valid(); it.next()) {
      const {
        trackIds: rawTrackIds,
        type,
        name,
        maxDepth,
        gpu_id: gpuId,
        machine_id: machineId,
        ugpu,
      } = it;
      const schema = schemas.get(type);
      if (schema === undefined) {
        continue;
      }
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const gpu =
        gpuId !== null ? new Gpu(ugpu ?? trackIds[0], gpuId, machineId) : null;
      const trackName = getTrackName({name, kind: SLICE_TRACK_KIND});
      const uri = `/slice_${ugpu ?? trackIds[0]}_${trackIds[0]}`;

      ctx.tracks.registerTrack({
        uri,
        tags: {
          kinds: [SLICE_TRACK_KIND],
          trackIds,
          type,
        },
        renderer: await createTraceProcessorSliceTrack({
          trace: ctx,
          uri,
          maxDepth,
          trackIds,
        }),
      });
      this.addToGpuGroup(
        ctx,
        schema.group,
        gpu,
        new TrackNode({uri, name: trackName, sortOrder: 0}),
      );
    }
  }
}
