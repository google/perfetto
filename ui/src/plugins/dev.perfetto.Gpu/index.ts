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
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {COUNTER_TRACK_KIND, SLICE_TRACK_KIND} from '../../public/track_kinds';
import {getMachineCount, getTrackName} from '../../public/utils';
import {TrackNode} from '../../public/workspace';
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';
import {TraceProcessorCounterTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_counter_track';
import {createTraceProcessorSliceTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_slice_track';
import TrackEventPlugin from '../dev.perfetto.TrackEvent';

// GPU frequency track that converts kHz values to Hz so that the generic
// counter renderer produces correct SI-prefixed labels (e.g. "2 GHz").
class GpuFreqTrack extends TraceProcessorCounterTrack {
  constructor(
    trace: Trace,
    uri: string,
    freqTrackId: number,
    trackName: string,
  ) {
    super({
      trace,
      uri,
      unit: 'Hz',
      trackId: freqTrackId,
      trackName,
      rootTable: 'counter',
      sqlSource: `
      select id, ts, value * 1000 as value, arg_set_id
      from counter
      where track_id = ${freqTrackId}
    `,
    });
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
export const SUMMARY_GROUP_SORT_BASE = 1000000;

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
//    are iterated — and thus inserted — in alphabetical order. Custom
//    counter group nodes are assigned incrementing positive sortOrder
//    values (starting above leaf tracks) based on alphabetical group
//    name, ensuring they also appear in alphabetical order among
//    themselves.
export default class GpuPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Gpu';
  static readonly dependencies = [
    StandardGroupsPlugin,
    TraceProcessorTrackPlugin,
    TrackEventPlugin,
  ];

  private groups = new Map<string, TrackNode>();
  private authoredMachineRoots = new Map<number, TrackNode>();
  private authoredGpuAnchors = new Map<string, TrackNode>();
  private gpuCount = 0;
  private numMachines = 0;

  async onTraceLoad(ctx: Trace): Promise<void> {
    const gpuCountResult = await ctx.engine.query(`
      select count(*) as cnt from gpu
    `);
    this.gpuCount = gpuCountResult.firstRow({cnt: NUM}).cnt;
    this.numMachines = await getMachineCount(ctx.engine);
    await this.discoverAuthoredGlobalGpuAnchors(ctx);

    await this.addGpuFreq(ctx);
    await this.addCounters(ctx);
    await this.addSlices(ctx);
  }

  private gpuAnchorKey(machineId: number, gpuId: number): string {
    return `${machineId}:${gpuId}`;
  }

  private async discoverAuthoredGlobalGpuAnchors(ctx: Trace): Promise<void> {
    const result = await ctx.engine.query(`
      WITH authored AS MATERIALIZED (
        SELECT
          child.min_track_id AS trackId,
          child.machine_id AS machineId,
          child.gpu_id AS gpuId,
          child.parent_id AS parentId,
          extract_arg(parent.dimension_arg_set_id, 'gpu') AS parentGpuId
        FROM _track_event_tracks_ordered_groups child
        LEFT JOIN track parent ON parent.id = child.parent_id
        WHERE child.scope = 'gpu'
          AND child.upid IS NULL
          AND child.gpu_hw_queue_iid IS NULL
          AND child.gpu_logical_queue_id IS NULL
      )
      SELECT trackId, machineId, gpuId
      FROM authored
      WHERE
        (gpuId IS NULL AND parentId IS NULL)
        OR
        (gpuId IS NOT NULL AND
          (parentId IS NULL OR parentGpuId IS NULL OR parentGpuId != gpuId))
      ORDER BY machineId, gpuId, trackId
    `);

    const trackEventPlugin = ctx.plugins.getPlugin(TrackEventPlugin);
    const machineCandidates = new Map<number, TrackNode | null>();
    const gpuCandidates = new Map<string, TrackNode | null>();
    const it = result.iter({
      trackId: NUM,
      machineId: NUM,
      gpuId: NUM_NULL,
    });
    for (; it.valid(); it.next()) {
      const node = trackEventPlugin.getTrackNode(it.trackId);
      if (node === undefined) continue;
      if (it.gpuId === null) {
        this.recordUniqueAnchor(machineCandidates, it.machineId, node);
      } else {
        this.recordUniqueAnchor(
          gpuCandidates,
          this.gpuAnchorKey(it.machineId, it.gpuId),
          node,
        );
      }
    }
    for (const [machineId, node] of machineCandidates) {
      if (node !== null) this.authoredMachineRoots.set(machineId, node);
    }
    for (const [key, node] of gpuCandidates) {
      if (node !== null) this.authoredGpuAnchors.set(key, node);
    }
  }

  private recordUniqueAnchor<K>(
    candidates: Map<K, TrackNode | null>,
    key: K,
    node: TrackNode,
  ): void {
    const existing = candidates.get(key);
    if (existing === undefined) {
      candidates.set(key, node);
    } else if (existing !== node) {
      candidates.set(key, null);
    }
  }

  private getAuthoredPlacement(
    machineId: number,
    gpuId: number | null,
  ): TrackNode | undefined {
    if (gpuId !== null) {
      return this.authoredGpuAnchors.get(this.gpuAnchorKey(machineId, gpuId));
    }
    return this.authoredMachineRoots.get(machineId);
  }

  private async addGpuFreq(ctx: Trace) {
    const result = await ctx.engine.query(`
      select
        gct.id,
        gct.gpu_id as gpuId,
        gct.machine_id as machineId,
        m.name as machineName,
        m.label_index as machineLabelIndex,
        gct.ugpu,
        g.name as gpuName
      from gpu_counter_track gct
      join _counter_track_summary using (id)
      left join gpu g on gct.ugpu = g.id
      left join machine m on m.id = gct.machine_id
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
      machineName: STR_NULL,
      machineLabelIndex: NUM_NULL,
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
          it.machineName ?? undefined,
          it.machineLabelIndex ?? undefined,
          this.numMachines,
        ),
      });
    }

    if (tracks.length === 0) return;

    const defaultTracks = tracks.filter(
      ({gpu}) => this.getAuthoredPlacement(gpu.machine, gpu.gpu) === undefined,
    );
    let defaultParent: TrackNode | undefined;
    if (defaultTracks.length > 0) {
      const gpuGroup = this.getGpuGroup(ctx);
      defaultParent =
        tracks.length > 1
          ? this.getGroupByName(gpuGroup, 'Frequency', null)
          : gpuGroup;
    }

    for (const {id, gpu} of tracks) {
      const uri = `/gpu_frequency_${gpu.ugpu}`;
      const authoredParent = this.getAuthoredPlacement(gpu.machine, gpu.gpu);
      const name =
        authoredParent !== undefined
          ? 'Frequency'
          : `${gpu.displayName} Frequency${gpu.maybeMachineLabel()}`;
      ctx.tracks.registerTrack({
        uri,
        tags: {
          kinds: [COUNTER_TRACK_KIND],
          trackIds: [id],
        },
        renderer: new GpuFreqTrack(ctx, uri, id, name),
      });
      const parent = authoredParent ?? defaultParent;
      if (parent === undefined) {
        throw new Error('GPU frequency track has no workspace parent');
      }
      parent.addChildInOrder(
        new TrackNode({
          uri,
          name,
          sortOrder: authoredParent !== undefined ? 0 : gpu.sortOrder,
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
          g.name as gpu_name,
          m.name as machine_name,
          m.label_index as machine_label_index
        from counter_track ct
        join _counter_track_summary using (id)
        left join gpu g on extract_arg(ct.dimension_arg_set_id, 'ugpu') = g.id
        left join machine m on m.id = ct.machine_id
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
      trackId: number;
      authoredParent: TrackNode | undefined;
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
      machine_name: STR_NULL,
      machine_label_index: NUM_NULL,
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
        machine_name: machineName,
        machine_label_index: machineLabelIndex,
      } = it;
      const schema = schemas.get(type);
      if (schema === undefined) {
        continue;
      }
      const gpu =
        gpuId !== null
          ? new Gpu(
              ugpu ?? trackId,
              gpuId,
              machineId,
              gpuName ?? undefined,
              machineName ?? undefined,
              machineLabelIndex ?? undefined,
              this.numMachines,
            )
          : null;
      const baseName = getTrackName({name, kind: COUNTER_TRACK_KIND});
      const authoredParent = this.getAuthoredPlacement(machineId, gpuId);
      let trackName = baseName;
      if (authoredParent !== undefined && schema.gpuTrackName !== undefined) {
        trackName = schema.gpuTrackName;
      } else if (gpu !== null && schema.gpuTrackName !== undefined) {
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
        renderer: new TraceProcessorCounterTrack({
          trace: ctx,
          uri,
          unit: unit ?? undefined,
          trackId,
          trackName,
        }),
      });

      counterTracks.push({
        schema,
        gpu,
        trackName,
        baseName,
        uri,
        trackId,
        authoredParent,
      });
    }

    // Query custom counter groups.
    const groupResult = await ctx.engine.query(`
      select
        group_id as groupId,
        name as groupName,
        track_id as trackId
      from gpu_counter_group
      where name is not null
      order by lower(name), group_id
    `);

    // Map trackId -> {groupId, groupName} for custom group assignment.
    // Also collect unique groups in alphabetical order for sort ordering.
    const trackGroupMap = new Map<
      number,
      {groupId: number; groupName: string}
    >();
    const uniqueGroups = new Map<number, string>();
    const groupIt = groupResult.iter({
      groupId: NUM,
      groupName: STR_NULL,
      trackId: NUM,
    });
    for (; groupIt.valid(); groupIt.next()) {
      if (groupIt.groupName !== null) {
        // Use the first named group for each track.
        if (!trackGroupMap.has(groupIt.trackId)) {
          trackGroupMap.set(groupIt.trackId, {
            groupId: groupIt.groupId,
            groupName: groupIt.groupName,
          });
        }
        if (!uniqueGroups.has(groupIt.groupId)) {
          uniqueGroups.set(groupIt.groupId, groupIt.groupName);
        }
      }
    }

    // Assign sort orders to custom groups: alphabetical, all above leaf
    // tracks so groups appear after ungrouped counter tracks.
    const CUSTOM_GROUP_SORT_BASE = 10000;
    const sortedGroupIds = [...uniqueGroups.entries()]
      .sort((a, b) => a[1].localeCompare(b[1]))
      .map(([id]) => id);
    const groupSortOrder = new Map<number, number>();
    for (let i = 0; i < sortedGroupIds.length; i++) {
      groupSortOrder.set(sortedGroupIds[i], CUSTOM_GROUP_SORT_BASE + i);
    }

    // Count ungrouped tracks per type to decide if a sub-group is needed,
    // matching the Frequency pattern: only create a sub-group when
    // there are multiple tracks of the same type.
    const ungroupedCounts = new Map<string, number>();
    for (const {schema, trackId} of counterTracks) {
      const customGroup = trackGroupMap.get(trackId);
      if (schema.group === undefined && customGroup === undefined) {
        ungroupedCounts.set(
          schema.type,
          (ungroupedCounts.get(schema.type) ?? 0) + 1,
        );
      }
    }

    for (const {
      schema,
      gpu,
      trackName,
      baseName,
      uri,
      trackId,
      authoredParent,
    } of counterTracks) {
      const trackNode = new TrackNode({
        uri,
        name: trackName,
        sortOrder: authoredParent !== undefined ? 0 : (gpu?.sortOrder ?? 0),
      });

      // A generic GPU placement anchor already identifies the GPU. Keep all
      // canonical telemetry as direct children and do not reproduce the
      // default Counters -> GPU N -> counter hierarchy.
      if (authoredParent !== undefined) {
        authoredParent.addChildInOrder(trackNode);
        continue;
      }

      // Check if this track has a custom group assignment.
      const customGroup = trackGroupMap.get(trackId);
      if (customGroup !== undefined && schema.type === 'gpu_counter') {
        const gpuGroup = this.getGpuGroup(ctx);
        const countersGroup = this.getGroupByName(
          gpuGroup,
          schema.group,
          null,
          schema.groupSortOrder,
        );

        // Place track under its custom group within Counters.
        let parent: TrackNode;
        const scopeId =
          gpu !== null
            ? gpu.machine > 0
              ? `${gpu.gpu}_m${gpu.machine}`
              : `${gpu.gpu}`
            : null;
        if (gpu !== null && this.gpuCount > 1) {
          const gpuSubGroupName = `${gpu.displayName} ${schema.group}${gpu.maybeMachineLabel()}`;
          parent = this.getGroupByName(
            countersGroup,
            gpuSubGroupName,
            scopeId,
            gpu.sortOrder,
          );
        } else {
          parent = countersGroup;
        }

        // Create custom group node using numeric ID as cache key.
        const key = `gpu_custom_group_${scopeId}_${customGroup.groupId}`;
        let groupNode = this.groups.get(key);
        if (!groupNode) {
          groupNode = new TrackNode({
            uri: `/gpu_group_${customGroup.groupId}`,
            isSummary: true,
            name: customGroup.groupName,
            collapsed: true,
            sortOrder:
              groupSortOrder.get(customGroup.groupId) ?? CUSTOM_GROUP_SORT_BASE,
          });
          parent.addChildInOrder(groupNode);
          this.groups.set(key, groupNode);
        }

        groupNode.addChildInOrder(trackNode);
        continue;
      }

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
        trackNode,
      );
    }
  }

  private async addSlices(ctx: Trace) {
    const sliceTypes = GPU_SLICE_SCHEMAS.map((s) => `'${s.type}'`).join(',');

    // A global hardware queue is removed from the default hierarchy only when
    // projection through an exact authored binding actually succeeded.
    await ctx.engine.query(`
      DROP TABLE IF EXISTS __gpu_global_bound_canonical_slices;
      CREATE PERFETTO TABLE __gpu_global_bound_canonical_slices AS
      SELECT DISTINCT extract_arg(
        projected.arg_set_id,
        'gpu_render_stage_canonical_slice_id'
      ) AS canonical_slice_id
      FROM slice projected
      JOIN track projected_track ON projected_track.id = projected.track_id
      WHERE projected_track.type GLOB 'gpu*_track_event'
        AND extract_arg(projected.arg_set_id,
                        'gpu_render_stage_canonical_slice_id') IS NOT NULL
        AND coalesce(
          extract_arg(projected_track.dimension_arg_set_id, 'upid'),
          extract_arg(projected_track.source_arg_set_id, 'gpu_process_upid')
        ) IS NULL;
      CREATE PERFETTO INDEX __gpu_global_bound_canonical_slices_idx
      ON __gpu_global_bound_canonical_slices(canonical_slice_id);

      DROP TABLE IF EXISTS __gpu_global_fully_bound_canonical_tracks;
      CREATE PERFETTO TABLE __gpu_global_fully_bound_canonical_tracks AS
      SELECT canonical.track_id AS track_id
      FROM gpu_slice canonical
      GROUP BY canonical.track_id
      HAVING count(*) = sum(EXISTS(
        SELECT 1
        FROM __gpu_global_bound_canonical_slices bound
        WHERE bound.canonical_slice_id = canonical.id
      ));
      CREATE PERFETTO INDEX __gpu_global_fully_bound_canonical_tracks_idx
      ON __gpu_global_fully_bound_canonical_tracks(track_id);
    `);

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
            g.name as gpu_name,
            m.name as machine_name,
            m.label_index as machine_label_index
          from _slice_track_summary s
          join track t using (id)
          left join gpu g on extract_arg(t.dimension_arg_set_id, 'ugpu') = g.id
          left join machine m on m.id = t.machine_id
          where t.type in (${sliceTypes})
            and not (
              t.type = 'gpu_render_stage'
              and exists (
                select 1
                from __gpu_global_fully_bound_canonical_tracks bound
                where bound.track_id = t.id
              )
            )
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
      machine_name: STR_NULL,
      machine_label_index: NUM_NULL,
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
        machine_name: machineName,
        machine_label_index: machineLabelIndex,
      } = it;
      const schema = schemas.get(type);
      if (schema === undefined) {
        continue;
      }
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const gpu =
        gpuId !== null
          ? new Gpu(
              ugpu ?? trackIds[0],
              gpuId,
              machineId,
              gpuName ?? undefined,
              machineName ?? undefined,
              machineLabelIndex ?? undefined,
              this.numMachines,
            )
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
      const authoredParent =
        type === 'gpu_render_stage'
          ? undefined
          : this.getAuthoredPlacement(machineId, gpuId);
      const trackNode = new TrackNode({
        uri,
        name: trackName,
        sortOrder: authoredParent !== undefined ? 0 : (gpu?.sortOrder ?? 0),
      });
      if (authoredParent !== undefined) {
        authoredParent.addChildInOrder(trackNode);
      } else {
        this.addToGpuGroup(
          ctx,
          schema.group,
          schema.groupSortOrder,
          gpu,
          trackNode,
        );
      }
    }
  }
}
