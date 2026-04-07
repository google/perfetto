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

// GPU frequency track that converts kHz values to Hz so that the generic
// counter renderer produces correct SI-prefixed labels (e.g. "2 GHz").
class GpuFreqTrack extends TraceProcessorCounterTrack {
  private readonly freqTrackId: number;

  constructor(
    trace: Trace,
    uri: string,
    freqTrackId: number,
    trackName: string,
  ) {
    super(trace, uri, {unit: 'Hz'}, freqTrackId, trackName);
    this.freqTrackId = freqTrackId;
  }

  override getSqlSource() {
    return `
      select id, ts, value * 1000 as value, arg_set_id
      from counter
      where track_id = ${this.freqTrackId}
    `;
  }
}

interface GpuCounterSchema {
  readonly type: string;
  readonly group: string | undefined;
  // Sort order for the top-level group under GPU. Groups with undefined
  // sortOrder default to 0 (top); summary-only groups use higher values.
  readonly groupSortOrder: number | undefined;
  // When set, the track is named "${gpu.displayName} ${gpuTrackName}" instead
  // of using the DB track name. This avoids redundant prefixes like
  // "GPU 0 GPU Memory" by allowing explicit control (e.g., "GPU 0 Memory").
  readonly gpuTrackName: string | undefined;
}

interface GpuSliceSchema {
  readonly type: string;
  readonly group: string | undefined;
  readonly groupSortOrder: number | undefined;
}

// Sort order base for summary-only groups (Counters, Hardware Queues, etc.)
// that should appear below leaf tracks (Frequency, Memory).
const SUMMARY_GROUP_SORT_BASE = 1000000;

const GPU_COUNTER_SCHEMAS: ReadonlyArray<GpuCounterSchema> = [
  {
    type: 'gpu_counter',
    group: 'Counters',
    groupSortOrder: SUMMARY_GROUP_SORT_BASE,
    gpuTrackName: undefined,
  },
  {
    type: 'gpu_memory',
    group: undefined,
    groupSortOrder: undefined,
    gpuTrackName: 'Memory',
  },
  {
    type: 'virtgpu_latency',
    group: 'Virtgpu Latency',
    groupSortOrder: SUMMARY_GROUP_SORT_BASE,
    gpuTrackName: undefined,
  },
  {
    type: 'virtgpu_num_free',
    group: 'Virtgpu num_free',
    groupSortOrder: SUMMARY_GROUP_SORT_BASE,
    gpuTrackName: undefined,
  },
  {
    type: 'vulkan_device_mem_allocation',
    group: 'Vulkan Allocations',
    groupSortOrder: SUMMARY_GROUP_SORT_BASE,
    gpuTrackName: undefined,
  },
  {
    type: 'vulkan_device_mem_bind',
    group: 'Vulkan Binds',
    groupSortOrder: SUMMARY_GROUP_SORT_BASE,
    gpuTrackName: undefined,
  },
  {
    type: 'vulkan_driver_mem',
    group: 'Vulkan Driver Memory',
    groupSortOrder: SUMMARY_GROUP_SORT_BASE,
    gpuTrackName: undefined,
  },
];

const GPU_SLICE_SCHEMAS: ReadonlyArray<GpuSliceSchema> = [
  {type: 'mali_mcu_state', group: undefined, groupSortOrder: undefined},
  {
    type: 'virtgpu_queue_event',
    group: 'Virtio GPU Events',
    groupSortOrder: SUMMARY_GROUP_SORT_BASE,
  },
  {
    type: 'gpu_render_stage',
    group: 'Hardware Queues',
    groupSortOrder: SUMMARY_GROUP_SORT_BASE,
  },
  {type: 'vulkan_events', group: undefined, groupSortOrder: undefined},
  {type: 'gpu_log', group: undefined, groupSortOrder: undefined},
  {type: 'graphics_frame_event', group: undefined, groupSortOrder: undefined},
];

// Track ordering
// ---------------
// GPU tracks are sorted using TrackNode.sortOrder combined with insertion
// order (addChildInOrder inserts before the first child with a strictly
// greater sortOrder; equal values preserve insertion order).
//
// 1. GPU identity: tracks for different GPUs are separated by
//    Gpu.sortOrder (= machine * MAX_GPUS_PER_MACHINE + gpu), so GPU 0
//    appears before GPU 1, and machines sort first.
// 2. Leaf before summary: leaf tracks (Frequency, Memory) use low
//    sortOrder values (>= 0). Summary-only groups (Counters, Hardware
//    Queues, etc.) use SUMMARY_GROUP_SORT_BASE so they appear below
//    leaf tracks.
// 3. Alphabetical: SQL queries use ORDER BY lower(name) so leaf tracks
//    are iterated — and thus inserted — in alphabetical order.
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
        gct.machine_id as machineId,
        gct.ugpu,
        g.name as gpuName
      from gpu_counter_track gct
      join _counter_track_summary using (id)
      left join gpu g on gct.ugpu = g.id
      where gct.name = 'gpufreq'
      order by machineId, gct.ugpu
    `);

    const tracks: Array<{
      id: number;
      gpu: Gpu;
    }> = [];
    const it = result.iter({
      id: NUM,
      gpuId: NUM,
      machineId: NUM,
      ugpu: NUM_NULL,
      gpuName: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      tracks.push({
        id: it.id,
        gpu: new Gpu(
          it.ugpu ?? it.gpuId,
          it.gpuId,
          it.machineId,
          it.gpuName ?? undefined,
        ),
      });
    }

    if (tracks.length === 0) return;

    const gpuGroup = this.getGpuGroup(ctx);

    // Only create a sub-group if there's more than one track.
    let parent: TrackNode;
    if (tracks.length > 1) {
      parent = this.getGroupByName(gpuGroup, 'Frequency', null);
    } else {
      parent = gpuGroup;
    }

    for (const {id, gpu} of tracks) {
      const uri = `/gpu_frequency_${gpu.ugpu}`;
      const name = `${gpu.displayName} Frequency${gpu.maybeMachineLabel()}`;
      ctx.tracks.registerTrack({
        uri,
        tags: {
          kinds: [COUNTER_TRACK_KIND],
          trackIds: [id],
        },
        renderer: new GpuFreqTrack(ctx, uri, id, name),
      });
      parent.addChildInOrder(
        new TrackNode({
          uri,
          name,
          sortOrder: gpu.sortOrder,
        }),
      );
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
    groupSortOrder: number | undefined,
    gpu: Gpu | null,
    track: TrackNode,
  ) {
    const gpuGroup = this.getGpuGroup(ctx);

    if (gpu !== null && group !== undefined && this.gpuCount > 1) {
      const parentGroup = this.getGroupByName(
        gpuGroup,
        group,
        null,
        groupSortOrder,
      );
      const gpuSubGroupName = `${gpu.displayName} ${group}${gpu.maybeMachineLabel()}`;
      const scopeId =
        gpu.machine > 0 ? `${gpu.gpu}_m${gpu.machine}` : `${gpu.gpu}`;
      const gpuSubGroup = this.getGroupByName(
        parentGroup,
        gpuSubGroupName,
        scopeId,
        gpu.sortOrder,
      );
      gpuSubGroup.addChildInOrder(track);
    } else {
      this.getGroupByName(
        gpuGroup,
        group,
        null,
        groupSortOrder,
      ).addChildInOrder(track);
    }
  }

  private getGroupByName(
    node: TrackNode,
    group: string | undefined,
    scopeId: string | null,
    sortOrder?: number,
  ): TrackNode {
    if (group === undefined) {
      return node;
    }
    const parentId = node.uri ?? 'root';
    const groupId = `gpu_group_${scopeId}_${parentId}_${group.toLowerCase().replace(' ', '_')}`;
    const groupNode = this.groups.get(groupId);
    if (groupNode) {
      return groupNode;
    }
    const newGroup = new TrackNode({
      uri: `/${group}`,
      isSummary: true,
      name: group,
      collapsed: true,
      sortOrder,
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
          extract_arg(ct.source_arg_set_id, 'description') as description,
          g.name as gpu_name
        from counter_track ct
        join _counter_track_summary using (id)
        left join gpu g on extract_arg(ct.dimension_arg_set_id, 'ugpu') = g.id
        where ct.type in (${counterTypes})
        order by ct.name
      )
      select * from tracks_summary
      order by lower(name)
    `);

    const schemas = new Map(GPU_COUNTER_SCHEMAS.map((x) => [x.type, x]));
    const counterTracks: Array<{
      schema: GpuCounterSchema;
      gpu: Gpu | null;
      trackName: string;
      baseName: string;
      uri: string;
    }> = [];
    const it = result.iter({
      id: NUM,
      type: STR,
      name: STR_NULL,
      unit: STR_NULL,
      gpu_id: NUM_NULL,
      machine_id: NUM,
      description: STR_NULL,
      ugpu: NUM_NULL,
      gpu_name: STR_NULL,
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
        gpu_name: gpuName,
      } = it;
      const schema = schemas.get(type);
      if (schema === undefined) {
        continue;
      }
      const gpu =
        gpuId !== null
          ? new Gpu(ugpu ?? trackId, gpuId, machineId, gpuName ?? undefined)
          : null;
      let trackName = getTrackName({name, kind: COUNTER_TRACK_KIND});
      if (gpu !== null && schema.gpuTrackName !== undefined) {
        trackName = `${gpu.displayName} ${schema.gpuTrackName}${gpu.maybeMachineLabel()}`;
      }
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

      counterTracks.push({
        schema,
        gpu,
        trackName,
        baseName: getTrackName({name, kind: COUNTER_TRACK_KIND}),
        uri,
      });
    }

    // Count ungrouped tracks per type to decide if a sub-group is needed,
    // matching the Frequency pattern: only create a sub-group when
    // there are multiple tracks of the same type.
    const ungroupedCounts = new Map<string, number>();
    for (const {schema} of counterTracks) {
      if (schema.group === undefined) {
        ungroupedCounts.set(
          schema.type,
          (ungroupedCounts.get(schema.type) ?? 0) + 1,
        );
      }
    }

    for (const {schema, gpu, trackName, baseName, uri} of counterTracks) {
      let group = schema.group;
      let groupGpu = gpu;
      if (group === undefined && (ungroupedCounts.get(schema.type) ?? 0) > 1) {
        // Multiple tracks of the same ungrouped type: create a sub-group
        // and add tracks directly under it without per-GPU sub-groups,
        // matching how addGpuFreq handles Frequency tracks.
        group = schema.gpuTrackName ?? baseName;
        groupGpu = null;
      }
      this.addToGpuGroup(
        ctx,
        group,
        schema.groupSortOrder,
        groupGpu,
        new TrackNode({
          uri,
          name: trackName,
          sortOrder: gpu?.sortOrder ?? 0,
        }),
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
            extract_arg(t.dimension_arg_set_id, 'gpu') as gpu_id,
            g.name as gpu_name
          from _slice_track_summary s
          join track t using (id)
          left join gpu g on extract_arg(t.dimension_arg_set_id, 'ugpu') = g.id
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
      gpu_name: STR_NULL,
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
        gpu_name: gpuName,
      } = it;
      const schema = schemas.get(type);
      if (schema === undefined) {
        continue;
      }
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const gpu =
        gpuId !== null
          ? new Gpu(ugpu ?? trackIds[0], gpuId, machineId, gpuName ?? undefined)
          : null;
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
        schema.groupSortOrder,
        gpu,
        new TrackNode({
          uri,
          name: trackName,
          sortOrder: gpu?.sortOrder ?? 0,
        }),
      );
    }
  }
}
