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

import {removeFalsyValues} from '../../base/array_utils';
import {TrackNode} from '../../public/workspace';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {getThreadUriPrefix, getTrackName} from '../../public/utils';
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
import {AsyncSliceTrack} from './async_slice_track';
import {
  getOrCreateGroupForProcess,
  getOrCreateGroupForThread,
} from '../../public/standard_groups';
import {exists} from '../../base/utils';
import {assertExists, assertTrue} from '../../base/logging';
import {SliceSelectionAggregator} from './slice_selection_aggregator';
import {sqlTableRegistry} from '../../frontend/widgets/sql/table/sql_table_registry';
import {getSliceTable} from './table';
import {extensions} from '../../public/lib/extensions';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.AsyncSlices';
  async onTraceLoad(ctx: Trace): Promise<void> {
    const trackIdsToUris = new Map<number, string>();

    await this.addGlobalAsyncTracks(ctx, trackIdsToUris);
    await this.addProcessAsyncSliceTracks(ctx, trackIdsToUris);
    await this.addThreadAsyncSliceTracks(ctx, trackIdsToUris);

    ctx.selection.registerSqlSelectionResolver({
      sqlTableName: 'slice',
      callback: async (id: number) => {
        // Locate the track for a given id in the slice table
        const result = await ctx.engine.query(`
          select
            track_id as trackId
          from
            slice
          where slice.id = ${id}
        `);

        if (result.numRows() === 0) {
          return undefined;
        }

        const {trackId} = result.firstRow({
          trackId: NUM,
        });

        const trackUri = trackIdsToUris.get(trackId);
        if (!trackUri) {
          return undefined;
        }

        return {
          trackUri,
          eventId: id,
        };
      },
    });

    ctx.selection.registerAreaSelectionAggreagtor(
      new SliceSelectionAggregator(),
    );

    sqlTableRegistry['slice'] = getSliceTable();

    ctx.commands.registerCommand({
      id: 'perfetto.ShowTable.slice',
      name: 'Open table: slice',
      callback: () => {
        extensions.addSqlTableTab(ctx, {
          table: getSliceTable(),
        });
      },
    });
  }

  async addGlobalAsyncTracks(
    ctx: Trace,
    trackIdsToUris: Map<number, string>,
  ): Promise<void> {
    const {engine} = ctx;
    // TODO(stevegolton): The track exclusion logic is currently a hack. This will be replaced
    // by a mechanism for more specific plugins to override tracks from more generic plugins.
    const suspendResumeLatencyTrackName = 'Suspend/Resume Latency';
    const rawGlobalAsyncTracks = await engine.query(`
      include perfetto module graphs.search;
      include perfetto module viz.summary.tracks;

      with global_tracks_grouped as (
        select
          t.parent_id,
          t.name,
          group_concat(id) as trackIds,
          count() as trackCount,
          min(a.order_id) as order_id
        from track t
        join _slice_track_summary using (id)
        left join _track_event_tracks_ordered a USING (id)
        where
          t.type in ('__intrinsic_track', 'gpu_track', '__intrinsic_cpu_track')
          and (name != '${suspendResumeLatencyTrackName}' or name is null)
          and classification not in (
            'linux_rpm',
            'linux_device_frequency',
            'irq_counter',
            'softirq_counter',
            'android_energy_estimation_breakdown',
            'android_energy_estimation_breakdown_per_uid'
          )
        group by parent_id, name
        order by parent_id, order_id
      ),
      intermediate_groups as (
        select
          t.name,
          t.id,
          t.parent_id
        from graph_reachable_dfs!(
          (
            select id as source_node_id, parent_id as dest_node_id
            from track
            where parent_id is not null
          ),
          (
            select distinct parent_id as node_id
            from global_tracks_grouped
            where parent_id is not null
          )
        ) g
        join track t on g.node_id = t.id
      )
      select
        t.name as name,
        t.parent_id as parentId,
        t.trackIds as trackIds,
        __max_layout_depth(t.trackCount, t.trackIds) as maxDepth
      from global_tracks_grouped t
      union all
      select
        t.name as name,
        t.parent_id as parentId,
        cast_string!(t.id) as trackIds,
        NULL as maxDepth
      from intermediate_groups t
      left join _slice_track_summary s using (id)
      where s.id is null
    `);
    const it = rawGlobalAsyncTracks.iter({
      name: STR_NULL,
      parentId: NUM_NULL,
      trackIds: STR,
      maxDepth: NUM_NULL,
    });

    // Create a map of track nodes by id
    const trackMap = new Map<
      number,
      {parentId: number | null; trackNode: TrackNode}
    >();

    for (; it.valid(); it.next()) {
      const rawName = it.name === null ? undefined : it.name;
      const title = getTrackName({
        name: rawName,
        kind: SLICE_TRACK_KIND,
      });
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const maxDepth = it.maxDepth;

      if (maxDepth === null) {
        assertTrue(trackIds.length == 1);
        const trackNode = new TrackNode({title, sortOrder: -25});
        trackMap.set(trackIds[0], {parentId: it.parentId, trackNode});
      } else {
        const uri = `/async_slices_${rawName}_${it.parentId}`;
        ctx.tracks.registerTrack({
          uri,
          title,
          tags: {
            trackIds,
            kind: SLICE_TRACK_KIND,
            scope: 'global',
          },
          track: new AsyncSliceTrack({trace: ctx, uri}, maxDepth, trackIds),
        });
        const trackNode = new TrackNode({
          uri,
          title,
          sortOrder: it.parentId === undefined ? -25 : 0,
        });
        trackIds.forEach((id) => {
          trackMap.set(id, {parentId: it.parentId, trackNode});
          trackIdsToUris.set(id, uri);
        });
      }
    }

    // Attach track nodes to parents / or the workspace if they have no parent
    trackMap.forEach(({parentId, trackNode}) => {
      if (exists(parentId)) {
        const parent = assertExists(trackMap.get(parentId));
        parent.trackNode.addChildInOrder(trackNode);
      } else {
        ctx.workspace.addChildInOrder(trackNode);
      }
    });
  }

  async addProcessAsyncSliceTracks(
    ctx: Trace,
    trackIdsToUris: Map<number, string>,
  ): Promise<void> {
    const result = await ctx.engine.query(`
      select
        upid,
        t.name as trackName,
        t.track_ids as trackIds,
        process.name as processName,
        process.pid as pid,
        t.parent_id as parentId,
        __max_layout_depth(t.track_count, t.track_ids) as maxDepth
      from _process_track_summary_by_upid_and_parent_id_and_name t
      join process using (upid)
      where t.name is null or t.name not glob "* Timeline"
    `);

    const it = result.iter({
      upid: NUM,
      parentId: NUM_NULL,
      trackName: STR_NULL,
      trackIds: STR,
      processName: STR_NULL,
      pid: NUM_NULL,
      maxDepth: NUM,
    });

    const trackMap = new Map<
      number,
      {parentId: number | null; upid: number; trackNode: TrackNode}
    >();

    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const trackName = it.trackName;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const processName = it.processName;
      const pid = it.pid;
      const maxDepth = it.maxDepth;

      const kind = SLICE_TRACK_KIND;
      const title = getTrackName({
        name: trackName,
        upid,
        pid,
        processName,
        kind,
      });

      const uri = `/process_${upid}/async_slices_${rawTrackIds}`;
      ctx.tracks.registerTrack({
        uri,
        title,
        tags: {
          trackIds,
          kind: SLICE_TRACK_KIND,
          scope: 'process',
          upid,
        },
        track: new AsyncSliceTrack({trace: ctx, uri}, maxDepth, trackIds),
      });
      const track = new TrackNode({uri, title, sortOrder: 30});
      trackIds.forEach((id) => {
        trackMap.set(id, {trackNode: track, parentId: it.parentId, upid});
        trackIdsToUris.set(id, uri);
      });
    }

    // Attach track nodes to parents / or the workspace if they have no parent
    trackMap.forEach((t) => {
      const parent = exists(t.parentId) && trackMap.get(t.parentId);
      if (parent !== false && parent !== undefined) {
        parent.trackNode.addChildInOrder(t.trackNode);
      } else {
        const processGroup = getOrCreateGroupForProcess(ctx.workspace, t.upid);
        processGroup.addChildInOrder(t.trackNode);
      }
    });
  }

  async addThreadAsyncSliceTracks(
    ctx: Trace,
    trackIdsToUris: Map<number, string>,
  ): Promise<void> {
    const result = await ctx.engine.query(`
      include perfetto module viz.summary.slices;
      include perfetto module viz.summary.threads;
      include perfetto module viz.threads;

      select
        t.utid,
        t.parent_id as parentId,
        thread.upid,
        t.name as trackName,
        thread.name as threadName,
        thread.tid as tid,
        t.track_ids as trackIds,
        __max_layout_depth(t.track_count, t.track_ids) as maxDepth,
        k.is_main_thread as isMainThread,
        k.is_kernel_thread AS isKernelThread
      from _thread_track_summary_by_utid_and_name t
      join _threads_with_kernel_flag k using(utid)
      join thread using (utid)
    `);

    const it = result.iter({
      utid: NUM,
      parentId: NUM_NULL,
      upid: NUM_NULL,
      trackName: STR_NULL,
      trackIds: STR,
      maxDepth: NUM,
      isMainThread: NUM_NULL,
      isKernelThread: NUM,
      threadName: STR_NULL,
      tid: NUM_NULL,
    });

    const trackMap = new Map<
      number,
      {parentId: number | null; utid: number; trackNode: TrackNode}
    >();

    for (; it.valid(); it.next()) {
      const {
        utid,
        parentId,
        upid,
        trackName,
        isMainThread,
        isKernelThread,
        maxDepth,
        threadName,
        tid,
      } = it;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const title = getTrackName({
        name: trackName,
        utid,
        tid,
        threadName,
        kind: 'Slices',
      });

      const uri = `/${getThreadUriPrefix(upid, utid)}_slice_${rawTrackIds}`;
      ctx.tracks.registerTrack({
        uri,
        title,
        tags: {
          trackIds,
          kind: SLICE_TRACK_KIND,
          scope: 'thread',
          utid,
          upid: upid ?? undefined,
          ...(isKernelThread === 1 && {kernelThread: true}),
        },
        chips: removeFalsyValues([
          isKernelThread === 0 && isMainThread === 1 && 'main thread',
        ]),
        track: new AsyncSliceTrack({trace: ctx, uri}, maxDepth, trackIds),
      });
      const track = new TrackNode({uri, title, sortOrder: 20});
      trackIds.forEach((id) => {
        trackMap.set(id, {trackNode: track, parentId, utid});
        trackIdsToUris.set(id, uri);
      });
    }

    // Attach track nodes to parents / or the workspace if they have no parent
    trackMap.forEach((t) => {
      const parent = exists(t.parentId) && trackMap.get(t.parentId);
      if (parent !== false && parent !== undefined) {
        parent.trackNode.addChildInOrder(t.trackNode);
      } else {
        const group = getOrCreateGroupForThread(ctx.workspace, t.utid);
        group.addChildInOrder(t.trackNode);
      }
    });
  }
}
