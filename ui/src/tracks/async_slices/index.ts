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

import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {getTrackName} from '../../public/utils';
import {
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';

import {AsyncSliceTrack} from './async_slice_track';
import {AsyncSliceTrackV2} from './async_slice_track_v2';

export const ASYNC_SLICE_TRACK_KIND = 'AsyncSliceTrack';

class AsyncSlicePlugin implements Plugin {
  onActivate(_ctx: PluginContext) {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    await this.addGlobalAsyncTracks(ctx);
    await this.addProcessAsyncSliceTracks(ctx);
    await this.addUserAsyncSliceTracks(ctx);
  }

  async addGlobalAsyncTracks(ctx: PluginContextTrace): Promise<void> {
    const {engine} = ctx;
    const rawGlobalAsyncTracks = await engine.query(`
      with tracks_with_slices as materialized (
        select distinct track_id
        from slice
      ),
      global_tracks as (
        select
          track.parent_id as parent_id,
          track.id as track_id,
          track.name as name
        from track
        join tracks_with_slices on tracks_with_slices.track_id = track.id
        where
          track.type = "track"
          or track.type = "gpu_track"
          or track.type = "cpu_track"
      ),
      global_tracks_grouped as (
        select
          parent_id,
          name,
          group_concat(track_id) as trackIds,
          count(track_id) as trackCount
        from global_tracks track
        group by parent_id, name
      )
      select
        t.parent_id as parentId,
        p.name as parentName,
        t.name as name,
        t.trackIds as trackIds,
        max_layout_depth(t.trackCount, t.trackIds) as maxDepth
      from global_tracks_grouped AS t
      left join track p on (t.parent_id = p.id)
      order by p.name, t.name;
    `);
    const it = rawGlobalAsyncTracks.iter({
      name: STR_NULL,
      parentName: STR_NULL,
      parentId: NUM_NULL,
      trackIds: STR,
      maxDepth: NUM_NULL,
    });

    // let scrollJankRendered = false;

    for (; it.valid(); it.next()) {
      const rawName = it.name === null ? undefined : it.name;
      // const rawParentName = it.parentName === null ? undefined :
      // it.parentName;
      const displayName =
          getTrackName({name: rawName, kind: ASYNC_SLICE_TRACK_KIND});
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      // const parentTrackId = it.parentId;
      const maxDepth = it.maxDepth;

      // If there are no slices in this track, skip it.
      if (maxDepth === null) {
        continue;
      }

      // if (ENABLE_SCROLL_JANK_PLUGIN_V2.get() && !scrollJankRendered &&
      //     name.includes(INPUT_LATENCY_TRACK)) {
      //   // This ensures that the scroll jank tracks render above the tracks
      //   // for GestureScrollUpdate.
      //   await this.addScrollJankTracks(this.engine);
      //   scrollJankRendered = true;
      // }

      ctx.registerTrack({
        uri: `perfetto.AsyncSlices#${rawName}.${it.parentId}`,
        displayName,
        trackIds,
        kind: ASYNC_SLICE_TRACK_KIND,
        track: ({trackKey}) => {
          return new AsyncSliceTrack(
              engine,
              maxDepth,
              trackKey,
              trackIds,
          );
        },
      });

      ctx.registerTrack({
        uri: `perfetto.AsyncSlices#${rawName}.${it.parentId}.v2`,
        displayName,
        trackIds,
        kind: ASYNC_SLICE_TRACK_KIND,
        track: ({trackKey}) => {
          return new AsyncSliceTrackV2(
              {engine, trackKey},
              maxDepth,
              trackIds,
          );
        },
      });
    }
  }

  async addProcessAsyncSliceTracks(ctx: PluginContextTrace): Promise<void> {
    const result = await ctx.engine.query(`
      with process_async_tracks as materialized (
        select
          process_track.upid as upid,
          process_track.name as trackName,
          process.name as processName,
          process.pid as pid,
          group_concat(process_track.id) as trackIds,
          count(1) as trackCount
        from process_track
        left join process using(upid)
        where
            process_track.name is null or
            process_track.name not like "% Timeline"
        group by
          process_track.upid,
          process_track.name
      )
      select
        t.*,
        max_layout_depth(t.trackCount, t.trackIds) as maxDepth
      from process_async_tracks t;
    `);

    const it = result.iter({
      upid: NUM,
      trackName: STR_NULL,
      trackIds: STR,
      processName: STR_NULL,
      pid: NUM_NULL,
      maxDepth: NUM_NULL,
    });
    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const trackName = it.trackName;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const processName = it.processName;
      const pid = it.pid;
      const maxDepth = it.maxDepth;

      if (maxDepth === null) {
        // If there are no slices in this track, skip it.
        continue;
      }

      const kind = ASYNC_SLICE_TRACK_KIND;
      const displayName =
          getTrackName({name: trackName, upid, pid, processName, kind});

      ctx.registerTrack({
        uri: `perfetto.AsyncSlices#process.${pid}${rawTrackIds}`,
        displayName,
        trackIds,
        kind: ASYNC_SLICE_TRACK_KIND,
        track: ({trackKey}) => {
          return new AsyncSliceTrack(
              ctx.engine,
              maxDepth,
              trackKey,
              trackIds,
          );
        },
      });

      ctx.registerTrack({
        uri: `perfetto.AsyncSlices#process.${pid}${rawTrackIds}.v2`,
        displayName,
        trackIds,
        kind: ASYNC_SLICE_TRACK_KIND,
        track: ({trackKey}) => {
          return new AsyncSliceTrackV2(
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
      with tracks_with_slices as materialized (
        select distinct track_id
        from slice
      ),
      global_tracks as (
        select
          uid_track.name,
          uid_track.uid,
          group_concat(uid_track.id) as trackIds,
          count(uid_track.id) as trackCount
        from uid_track
        join tracks_with_slices
        where tracks_with_slices.track_id == uid_track.id
        group by uid_track.uid
      )
      select
        t.name as name,
        t.uid as uid,
        package_list.package_name as package_name,
        t.trackIds as trackIds,
        max_layout_depth(t.trackCount, t.trackIds) as maxDepth
      from global_tracks t
      join package_list
      where t.uid = package_list.uid
      group by t.uid
      `);

    const it = result.iter({
      name: STR_NULL,
      uid: NUM_NULL,
      package_name: STR_NULL,
      trackIds: STR,
      maxDepth: NUM_NULL,
    });

    for (; it.valid(); it.next()) {
      const kind = ASYNC_SLICE_TRACK_KIND;
      const rawName = it.name === null ? undefined : it.name;
      const userName = it.package_name === null ? undefined : it.package_name;
      const uid = it.uid === null ? undefined : it.uid;
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
        track: ({trackKey}) => {
          return new AsyncSliceTrack(
              engine,
              maxDepth,
              trackKey,
              trackIds,
          );
        },
      });

      ctx.registerTrack({
        uri: `perfetto.AsyncSlices#${rawName}.${uid}.v2`,
        displayName,
        trackIds,
        kind: ASYNC_SLICE_TRACK_KIND,
        track: ({trackKey}) => {
          return new AsyncSliceTrackV2(
              {engine, trackKey},
              maxDepth,
              trackIds,
          );
        },
      });
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.AsyncSlices',
  plugin: AsyncSlicePlugin,
};
