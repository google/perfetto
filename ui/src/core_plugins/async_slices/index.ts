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

import {ASYNC_SLICE_TRACK_KIND} from '../../public';
import {Plugin, PluginContextTrace, PluginDescriptor} from '../../public';
import {getTrackName} from '../../public/utils';
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';

import {AsyncSliceTrack} from './async_slice_track';

class AsyncSlicePlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    await this.addGlobalAsyncTracks(ctx);
    await this.addProcessAsyncSliceTracks(ctx);
    await this.addUserAsyncSliceTracks(ctx);
  }

  async addGlobalAsyncTracks(ctx: PluginContextTrace): Promise<void> {
    const {engine} = ctx;
    const rawGlobalAsyncTracks = await engine.query(`
      with global_tracks_grouped as (
        select
          parent_id,
          name,
          group_concat(id) as trackIds,
          count() as trackCount
        from track t
        join _slice_track_summary using (id)
        where t.type in ('track', 'gpu_track', 'cpu_track')
        group by parent_id, name
      )
      select
        t.name as name,
        t.parent_id as parentId,
        t.trackIds as trackIds,
        __max_layout_depth(t.trackCount, t.trackIds) as maxDepth
      from global_tracks_grouped t
    `);
    const it = rawGlobalAsyncTracks.iter({
      name: STR_NULL,
      parentId: NUM_NULL,
      trackIds: STR,
      maxDepth: NUM,
    });

    for (; it.valid(); it.next()) {
      const rawName = it.name === null ? undefined : it.name;
      const displayName = getTrackName({
        name: rawName,
        kind: ASYNC_SLICE_TRACK_KIND,
      });
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const maxDepth = it.maxDepth;

      ctx.registerTrack({
        uri: `perfetto.AsyncSlices#${rawName}.${it.parentId}`,
        displayName,
        trackIds,
        kind: ASYNC_SLICE_TRACK_KIND,
        trackFactory: ({trackKey}) => {
          return new AsyncSliceTrack({engine, trackKey}, maxDepth, trackIds);
        },
      });
    }
  }

  async addProcessAsyncSliceTracks(ctx: PluginContextTrace): Promise<void> {
    const result = await ctx.engine.query(`
      select
        upid,
        t.name as trackName,
        t.track_ids as trackIds,
        process.name as processName,
        process.pid as pid,
        __max_layout_depth(t.track_count, t.track_ids) as maxDepth
      from _process_track_summary_by_upid_and_name t
      join process using(upid)
      where t.name is null or t.name not glob "* Timeline"
    `);

    const it = result.iter({
      upid: NUM,
      trackName: STR_NULL,
      trackIds: STR,
      processName: STR_NULL,
      pid: NUM_NULL,
      maxDepth: NUM,
    });
    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const trackName = it.trackName;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const processName = it.processName;
      const pid = it.pid;
      const maxDepth = it.maxDepth;

      const kind = ASYNC_SLICE_TRACK_KIND;
      const displayName = getTrackName({
        name: trackName,
        upid,
        pid,
        processName,
        kind,
      });

      ctx.registerTrack({
        uri: `perfetto.AsyncSlices#process.${pid}${rawTrackIds}`,
        displayName,
        trackIds,
        kind: ASYNC_SLICE_TRACK_KIND,
        trackFactory: ({trackKey}) => {
          return new AsyncSliceTrack(
            {engine: ctx.engine, trackKey},
            maxDepth,
            trackIds,
          );
        },
      });
    }
  }

  async addUserAsyncSliceTracks(ctx: PluginContextTrace): Promise<void> {
    const {engine} = ctx;
    const result = await engine.query(`
      with grouped_packages as materialized (
        select
          uid,
          group_concat(package_name, ',') as package_name,
          count() as cnt
        from package_list
        group by uid
      )
      select
        t.name as name,
        t.uid as uid,
        t.track_ids as trackIds,
        __max_layout_depth(t.track_count, t.track_ids) as maxDepth,
        iif(g.cnt = 1, g.package_name, 'UID ' || g.uid) as packageName
      from _uid_track_track_summary_by_uid_and_name t
      left join grouped_packages g using (uid)
    `);

    const it = result.iter({
      name: STR_NULL,
      uid: NUM_NULL,
      packageName: STR_NULL,
      trackIds: STR,
      maxDepth: NUM_NULL,
    });

    for (; it.valid(); it.next()) {
      const kind = ASYNC_SLICE_TRACK_KIND;
      const rawName = it.name === null ? undefined : it.name;
      const uid = it.uid === null ? undefined : it.uid;
      const userName = it.packageName === null ? `UID ${uid}` : it.packageName;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const maxDepth = it.maxDepth;

      // If there are no slices in this track, skip it.
      if (maxDepth === null) {
        continue;
      }

      const displayName = getTrackName({
        name: rawName,
        uid,
        userName,
        kind,
        uidTrack: true,
      });

      ctx.registerTrack({
        uri: `perfetto.AsyncSlices#${rawName}.${uid}`,
        displayName,
        trackIds,
        kind: ASYNC_SLICE_TRACK_KIND,
        trackFactory: ({trackKey}) => {
          return new AsyncSliceTrack({engine, trackKey}, maxDepth, trackIds);
        },
      });
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.AsyncSlices',
  plugin: AsyncSlicePlugin,
};
